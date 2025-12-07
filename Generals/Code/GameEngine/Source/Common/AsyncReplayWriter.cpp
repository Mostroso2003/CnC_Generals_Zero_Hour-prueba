/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  AsyncReplayWriter Implementation                                         //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"
#include "Common/AsyncReplayWriter.h"
#include "Common/Debug.h"

/**
 * Constructor - Initialize thread and synchronization primitives
 */
AsyncReplayWriter::AsyncReplayWriter()
	: m_file(NULL)
	, m_writerThread(NULL)
	, m_wakeEvent(NULL)
	, m_running(FALSE)
	, m_shouldExit(FALSE)
	, m_totalWrites(0)
	, m_totalBytesWritten(0)
	, m_peakQueueSize(0)
{
	// Initialize critical section for thread-safe queue access
	InitializeCriticalSection(&m_queueLock);

	// Create manual-reset event for thread wake-up
	m_wakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset event
	DEBUG_ASSERTCRASH(m_wakeEvent != NULL, ("Failed to create wake event"));

	// Start writer thread
	m_writerThread = CreateThread(
		NULL,                   // Security attributes
		0,                      // Stack size (default)
		writerThreadProc,       // Thread function
		this,                   // Parameter to thread function
		0,                      // Creation flags
		NULL                    // Thread ID
	);

	DEBUG_ASSERTCRASH(m_writerThread != NULL, ("Failed to create async writer thread"));

	// Set thread priority to below normal (I/O work)
	SetThreadPriority(m_writerThread, THREAD_PRIORITY_BELOW_NORMAL);

	m_running = TRUE;

	DEBUG_LOG(("AsyncReplayWriter: Initialized successfully\n"));
}

/**
 * Destructor - Clean shutdown of thread and resources
 */
AsyncReplayWriter::~AsyncReplayWriter()
{
	// Signal thread to exit
	m_shouldExit = TRUE;
	SetEvent(m_wakeEvent);

	// Wait for thread to finish (max 5 seconds)
	if (m_writerThread != NULL) {
		DWORD waitResult = WaitForSingleObject(m_writerThread, 5000);
		if (waitResult == WAIT_TIMEOUT) {
			DEBUG_LOG(("AsyncReplayWriter: Thread did not exit cleanly, forcing termination\n"));
			TerminateThread(m_writerThread, 1);
		}
		CloseHandle(m_writerThread);
		m_writerThread = NULL;
	}

	// Close event
	if (m_wakeEvent != NULL) {
		CloseHandle(m_wakeEvent);
		m_wakeEvent = NULL;
	}

	// Clean up any remaining commands in queue
	EnterCriticalSection(&m_queueLock);
	while (!m_writeQueue.empty()) {
		ReplayWriteCommand* cmd = m_writeQueue.front();
		m_writeQueue.pop();
		delete cmd;
	}
	LeaveCriticalSection(&m_queueLock);

	// Delete critical section
	DeleteCriticalSection(&m_queueLock);

	// Close file if still open
	if (m_file != NULL) {
		fclose(m_file);
		m_file = NULL;
	}

	DEBUG_LOG(("AsyncReplayWriter: Shutdown complete. Stats - Writes: %d, Bytes: %d, Peak Queue: %d\n",
		m_totalWrites, m_totalBytesWritten, m_peakQueueSize));
}

/**
 * Open file for writing (called from main thread)
 */
Bool AsyncReplayWriter::openFile(const char* filename)
{
	DEBUG_ASSERTCRASH(filename != NULL, ("Null filename passed to openFile"));

	// Close existing file if open
	if (m_file != NULL) {
		closeFile();
	}

	m_filename = filename;

	// File will be opened on writer thread on first write
	// This avoids blocking the main thread
	DEBUG_LOG(("AsyncReplayWriter: Queued file open for %s\n", filename));

	return TRUE;
}

/**
 * Close file (called from main thread)
 */
void AsyncReplayWriter::closeFile()
{
	if (m_filename.isEmpty()) {
		return;
	}

	// Enqueue close command
	ReplayWriteCommand* cmd = NEW ReplayWriteCommand();
	cmd->type = ReplayWriteCommand::CMD_CLOSE;

	EnterCriticalSection(&m_queueLock);
	m_writeQueue.push(cmd);
	LeaveCriticalSection(&m_queueLock);

	// Wake writer thread
	SetEvent(m_wakeEvent);

	// Wait a bit for queue to drain (non-blocking wait)
	for (int i = 0; i < 50 && getPendingWrites() > 0; ++i) {
		Sleep(10);  // 10ms * 50 = 500ms max wait
	}

	m_filename.clear();
}

/**
 * Write data to file (called from main thread - NON-BLOCKING)
 */
void AsyncReplayWriter::writeData(const void* data, size_t size)
{
	DEBUG_ASSERTCRASH(data != NULL && size > 0, ("Invalid write data"));

	// Copy data to heap (will be freed by worker thread)
	UnsignedByte* dataCopy = NEW UnsignedByte[size];
	memcpy(dataCopy, data, size);

	// Create write command
	ReplayWriteCommand* cmd = NEW ReplayWriteCommand();
	cmd->type = ReplayWriteCommand::CMD_WRITE_DATA;
	cmd->data = dataCopy;
	cmd->dataSize = size;

	// Enqueue command (critical section)
	EnterCriticalSection(&m_queueLock);

	// Check queue size limit
	if (m_writeQueue.size() >= MAX_QUEUE_SIZE) {
		DEBUG_LOG(("AsyncReplayWriter: WARNING - Queue full (%d), dropping write!\n", MAX_QUEUE_SIZE));
		delete cmd;
		LeaveCriticalSection(&m_queueLock);
		return;
	}

	m_writeQueue.push(cmd);

	// Track peak queue size
	UnsignedInt queueSize = (UnsignedInt)m_writeQueue.size();
	if (queueSize > m_peakQueueSize) {
		m_peakQueueSize = queueSize;
	}

	LeaveCriticalSection(&m_queueLock);

	// Wake writer thread
	SetEvent(m_wakeEvent);
}

/**
 * Seek to position in file (called from main thread)
 */
void AsyncReplayWriter::seek(UnsignedInt offset, Int origin)
{
	ReplayWriteCommand* cmd = NEW ReplayWriteCommand();
	cmd->type = ReplayWriteCommand::CMD_SEEK;
	cmd->seekOffset = offset;
	cmd->seekOrigin = origin;

	EnterCriticalSection(&m_queueLock);
	m_writeQueue.push(cmd);
	LeaveCriticalSection(&m_queueLock);

	SetEvent(m_wakeEvent);
}

/**
 * Flush file buffer (called from main thread - NON-BLOCKING)
 */
void AsyncReplayWriter::flush()
{
	ReplayWriteCommand* cmd = NEW ReplayWriteCommand();
	cmd->type = ReplayWriteCommand::CMD_FLUSH;

	EnterCriticalSection(&m_queueLock);
	m_writeQueue.push(cmd);
	LeaveCriticalSection(&m_queueLock);

	SetEvent(m_wakeEvent);
}

/**
 * Get number of pending writes (thread-safe)
 */
UnsignedInt AsyncReplayWriter::getPendingWrites() const
{
	EnterCriticalSection((LPCRITICAL_SECTION)&m_queueLock);
	UnsignedInt size = (UnsignedInt)m_writeQueue.size();
	LeaveCriticalSection((LPCRITICAL_SECTION)&m_queueLock);
	return size;
}

/**
 * Writer thread entry point (static)
 */
DWORD WINAPI AsyncReplayWriter::writerThreadProc(LPVOID param)
{
	AsyncReplayWriter* writer = (AsyncReplayWriter*)param;
	DEBUG_ASSERTCRASH(writer != NULL, ("Null writer in thread proc"));

	writer->writerThreadMain();

	return 0;
}

/**
 * Writer thread main loop
 */
void AsyncReplayWriter::writerThreadMain()
{
	DEBUG_LOG(("AsyncReplayWriter: Writer thread started\n"));

	while (!m_shouldExit) {
		// Wait for wake event (up to 100ms)
		DWORD waitResult = WaitForSingleObject(m_wakeEvent, 100);

		if (waitResult == WAIT_OBJECT_0 || !m_writeQueue.empty()) {
			// Process all pending commands
			processQueue();
		}
	}

	// Final queue drain on exit
	processQueue();

	// Close file if still open
	if (m_file != NULL) {
		fflush(m_file);
		fclose(m_file);
		m_file = NULL;
	}

	DEBUG_LOG(("AsyncReplayWriter: Writer thread exiting\n"));
}

/**
 * Process write queue (runs on writer thread)
 */
void AsyncReplayWriter::processQueue()
{
	while (true) {
		ReplayWriteCommand* cmd = NULL;

		// Pop command from queue
		EnterCriticalSection(&m_queueLock);
		if (!m_writeQueue.empty()) {
			cmd = m_writeQueue.front();
			m_writeQueue.pop();
		}
		LeaveCriticalSection(&m_queueLock);

		if (cmd == NULL) {
			break;  // Queue empty
		}

		// Execute command
		executeCommand(cmd);

		// Delete command
		delete cmd;
	}
}

/**
 * Execute a single write command (runs on writer thread)
 */
void AsyncReplayWriter::executeCommand(ReplayWriteCommand* cmd)
{
	DEBUG_ASSERTCRASH(cmd != NULL, ("Null command"));

	switch (cmd->type) {
		case ReplayWriteCommand::CMD_WRITE_DATA:
		{
			if (m_file == NULL && !m_filename.isEmpty()) {
				// Lazy file open on first write
				m_file = fopen(m_filename.str(), "wb");
				if (m_file == NULL) {
					DEBUG_LOG(("AsyncReplayWriter: ERROR - Failed to open file %s\n", m_filename.str()));
					return;
				}
			}

			if (m_file != NULL && cmd->data != NULL && cmd->dataSize > 0) {
				size_t written = fwrite(cmd->data, 1, cmd->dataSize, m_file);
				if (written != cmd->dataSize) {
					DEBUG_LOG(("AsyncReplayWriter: WARNING - Partial write (%d/%d bytes)\n", written, cmd->dataSize));
				}

				m_totalWrites++;
				m_totalBytesWritten += written;
			}
			break;
		}

		case ReplayWriteCommand::CMD_SEEK:
		{
			if (m_file != NULL) {
				fseek(m_file, cmd->seekOffset, cmd->seekOrigin);
			}
			break;
		}

		case ReplayWriteCommand::CMD_FLUSH:
		{
			if (m_file != NULL) {
				fflush(m_file);  // This is OK on worker thread
			}
			break;
		}

		case ReplayWriteCommand::CMD_CLOSE:
		{
			if (m_file != NULL) {
				fflush(m_file);
				fclose(m_file);
				m_file = NULL;
				DEBUG_LOG(("AsyncReplayWriter: File closed\n"));
			}
			break;
		}

		default:
			DEBUG_LOG(("AsyncReplayWriter: WARNING - Unknown command type %d\n", cmd->type));
			break;
	}
}
