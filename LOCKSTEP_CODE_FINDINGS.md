# HALLAZGOS CRÍTICOS POR ARCHIVO
## Sistema Lockstep - Sincronización Multijugador

---

## ARCHIVO 1: Network.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameNetwork/Network.cpp`

### Hallazgo 1.1: Punto de Bloqueo - AllCommandsReady()
**Línea:** 716
**Severidad:** CRÍTICA

```cpp
if (AllCommandsReady(TheGameLogic->getFrame())) { // Si todos están listos...
    m_conMgr->handleAllCommandsReady();
    if (timeForNewFrame()) { 
        RelayCommandsToCommandList(TheGameLogic->getFrame());
        m_frameDataReady = TRUE; // SOLO SI ESTA CONDICIÓN ES TRUE
    }
}
```

**Impacto:** 
- Si AllCommandsReady() retorna FALSE, m_frameDataReady permanece FALSE
- GameLogic no avanza (ver GameEngine.cpp línea 594)
- El juego se congela esperando al jugador más lento

### Hallazgo 1.2: Reset de Estado de Frame
**Línea:** 692
**Severidad:** MEDIA

```cpp
m_frameDataReady = FALSE;  // Se RESETEA cada frame
```

**Implicación:**
- Necesario que Network::update() lo establezca a TRUE cada frame
- Si no se establece, el juego no progresa
- Ciclo de espera: Network::update() -> AllCommandsReady() -> FALSE -> Espera

### Hallazgo 1.3: Control de Run-Ahead
**Línea:** 336
**Severidad:** MEDIA

```cpp
m_runAhead = min(max(30, MIN_RUNAHEAD), MAX_FRAMES_AHEAD/2);
// = min(max(30, 10), 64) = 30 frames por defecto
```

**Tabla de Run-Ahead:**
```
MIN_RUNAHEAD = 10  (valor mínimo)
DEFAULT = 30       (valor inicial)
MAX = 64           (MAX_FRAMES_AHEAD/2)
```

Con 30 fps: 30 frames = 1 segundo de buffer entre jugadores

### Hallazgo 1.4: Función isFrameDataReady()
**Línea:** 807-809
**Severidad:** ALTA

```cpp
Bool Network::isFrameDataReady() {
    return (m_frameDataReady || (m_localStatus == NETLOCALSTATUS_LEFT));
}
```

**Explicación:**
- Es la PUERTA de control para GameLogic::UPDATE()
- Retorna TRUE solo si:
  - m_frameDataReady = TRUE (establecido en línea 721), O
  - El jugador ya salió del juego
- Todo el juego depende de esto

### Hallazgo 1.5: Procesamiento de Run-Ahead Dinámico
**Línea:** 703
**Severidad:** MEDIA

```cpp
m_conMgr->updateRunAhead(m_runAhead, m_frameRate, 
                         m_didSelfSlug, getExecutionFrame());
```

**Lo que hace:**
- Ajusta dinámicamente el run-ahead basado en latencia y FPS de jugadores
- Si detecta que el cushion es muy pequeño, reduce la velocidad ("self slug")
- Afecta a TODOS los jugadores si uno se atrasa

### Hallazgo 1.6: Loop Principal del Update
**Línea:** 684-724
**Severidad:** ALTA

**Flujo completo:**
```
Network::update() {
    1. Reset m_frameDataReady = FALSE (L692)
    2. GetCommandsFromCommandList() (L700)
    3. updateRunAhead() si en juego (L703)
    4. liteupdate() procesa red (L710)
    5. AllCommandsReady()? (L716) <-- PUNTO CRÍTICO
       - SI: handleAllCommandsReady() + timeForNewFrame() + RelayCommands()
       - NO: ESPERA
}
```

**Gráfico de Flujo de Espera:**
```
┌─────────────────────────────────┐
│ Network::update()               │
├─────────────────────────────────┤
│ AllCommandsReady(frame)?        │
├──────────┬──────────────────────┤
│ NO       │ YES                  │
├──────────┼──────────────────────┤
│ESPERA    │ handleAllCommandsReady() (L717)
│CONGELADO │ timeForNewFrame()? (L719)
│          │   YES: RelayCommands() + ready=TRUE (L720-721)
│          │   NO: throttle/esperar
└──────────┴──────────────────────┘
```

---

## ARCHIVO 2: ConnectionManager.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameNetwork/ConnectionManager.cpp`

### Hallazgo 2.1: Loop Critical - allCommandsReady()
**Línea:** 1556-1596
**Severidad:** CRÍTICA

```cpp
Bool ConnectionManager::allCommandsReady(UnsignedInt frame, Bool justTesting) {
    Bool retval = TRUE;
    
    // LOOP O(n) donde n = MAX_SLOTS = 8
    for (Int i = 0; (i < MAX_SLOTS) && retval; ++i) {  // Línea 1556
        if ((m_frameData[i] != NULL) && 
            (m_frameData[i]->getIsQuitting() == FALSE)) {
            
            // Obtiene estado de comandos para cada jugador
            frameRetVal = m_frameData[i]->allCommandsReady(frame, ...);  // L1570
            
            if (frameRetVal == FRAMEDATA_NOTREADY) {
                retval = FALSE;  // BLOQUEO: Falta un jugador
            } else if (frameRetVal == FRAMEDATA_RESEND) {
                requestFrameDataResend(i, frame);  // Pide resend
                retval = FALSE;  // BLOQUEO: Error en sincronización
            }
        }
    }
    
    // Si TODOS están listos, chequea DisconnectManager
    if ((retval == TRUE) && (justTesting == FALSE)) {
        m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this);  // L1590
        retval = m_disconnectManager->allowedToContinue();  // L1591
    }
    
    return retval;
}
```

**Análisis de Bloqueo:**
```
for (8 jugadores) {
    if (jugador_no_listo) {
        return FALSE;  // INMEDIATO - No espera a otros
    }
}
```

- El loop se detiene tan pronto como encuentra un jugador sin comandos
- Es la razón por la que 1 jugador lento congelota a TODOS

### Hallazgo 2.2: Frame Data Resend - Limpieza O(n)
**Línea:** 1580-1587
**Severidad:** ALTA

```cpp
if (frameRetVal == FRAMEDATA_RESEND) {
    // Frame data está corrupto, limpiar para resend
    for (i = 0; i < MAX_SLOTS; ++i) {  // O(n)
        if ((m_frameData[i] != NULL) && (i != m_localSlot)) {
            m_frameData[i]->resetFrame(frame, FALSE);
        }
    }
}
```

**Impacto:**
- Cuando falla sincronización, limpia datos de TODOS los jugadores
- O(n) operación cada vez que hay error
- En partidas inestables, sucede frecuentemente

### Hallazgo 2.3: updateRunAhead() - Cálculo de Latencia
**Línea:** 1213-1350
**Severidad:** MEDIA

```cpp
void ConnectionManager::updateRunAhead(...) {
    static time_t lasttimesent = 0;
    time_t curTime = timeGetTime();
    
    // Actualiza solo cada m_networkRunAheadMetricsTime ms (configurado)
    if ((lasttimesent == 0) || 
        ((curTime - lasttimesent) > TheGlobalData->m_networkRunAheadMetricsTime)) {
        
        if (m_localSlot == m_packetRouterSlot) {  // Solo packet router
            // Calcula nuevo run-ahead
            Int newRunAhead = (Int)((getMaximumLatency() / 2.0) * (Real)minFps);  // L1249
            newRunAhead += (newRunAhead * TheGlobalData->m_networkRunAheadSlack) / 100;  // L1250
            
            // Cap máximo
            if (newRunAhead > (MAX_FRAMES_AHEAD / 2)) {  // L1255
                newRunAhead = MAX_FRAMES_AHEAD / 2;
            }
            
            // Envía a otros jugadores
            sendLocalCommand(msg, 0xff ^ (1 << minFpsPlayer));  // L1284
        }
    }
}
```

**Fórmula de Run-Ahead:**
```
newRunAhead = (max_latency_sec / 2.0) × min_fps
            + (newRunAhead × slack%) 
            
Ejemplo:
- max_latency = 0.5 sec (500ms)
- min_fps = 30
- slack = 10%

newRunAhead = (0.5 / 2.0) × 30 + (7.5 × 10%)
            = 0.25 × 30 + 0.75
            = 7.5 + 0.75
            = 8.25 frames (BAJO!)
```

### Hallazgo 2.4: Throttling por "Self Slug"
**Línea:** 1229-1283
**Severidad:** MEDIA

```cpp
if (didSelfSlug) {
    // Si el juego local se ralentizó a sí mismo
    // por estar muy cercano al límite de run-ahead
    m_fpsAverages[m_localSlot] = frameRate;  // Reporta FPS reducido
}

// Envía comando de ajuste de run-ahead
msg->setFrameRate(minFps);  // L1282
```

**Implicación:**
- Si un jugador llama "self slug", reporta FPS bajo
- Todos los demás ajustan su run-ahead al FPS más bajo
- TODOS se desaceleran aunque solo uno lo necesite

### Hallazgo 2.5: Comando de Desconexión de Reproductor
**Línea:** 1650-1656
**Severidad:** MEDIA

```cpp
// Cuando un jugador sale del juego
for (Int i = 0; i < MAX_SLOTS; ++i) {  // O(n)
    if (m_connections[i] != NULL) {
        m_connections[i]->setFrameGrouping(frameGrouping);
    }
}
```

**Uso de memoria:** MAX_SLOTS conexiones simultáneas
**Costo:** O(n) cada vez que cambia algún parámetro de red

---

## ARCHIVO 3: DisconnectManager.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameNetwork/DisconnectManager.cpp`

### Hallazgo 3.1: Votación O(n²) - Constructor
**Línea:** 65-70
**Severidad:** ALTA

```cpp
for( i = 0; i < MAX_SLOTS; ++i) {
    for (Int j = 0; j < MAX_SLOTS; ++j) {  // ANIDADO: O(n²)
        m_playerVotes[i][j].vote = FALSE;
        m_playerVotes[i][j].frame = 0;
    }
}
```

**Análisis:**
```
Operaciones: 8 × 8 = 64 iteraciones
Estructura: m_playerVotes[8][8] (matriz)
Frecuencia: Solo en init/destructor (no es per-frame)
Memoria: ~256 bytes
```

### Hallazgo 3.2: Votación O(n²) - init()
**Línea:** 85-90
**Severidad:** MEDIA

```cpp
void DisconnectManager::init() {
    // ...
    for (Int i = 0; i < MAX_SLOTS; ++i) {
        for (Int j = 0; j < MAX_SLOTS; ++j) {  // O(n²) otra vez
            m_playerVotes[i][j].vote = FALSE;
            m_playerVotes[i][j].frame = 0;
        }
    }
}
```

**Impacto:** Se ejecuta cuando se inicia el desconect manager cada partida

### Hallazgo 3.3: Conteo de Votos - O(n)
**Línea:** 776-789
**Severidad:** MEDIA

```cpp
Int DisconnectManager::countVotesForPlayer(Int slot) {
    if ((slot < 0) || (slot >= MAX_SLOTS)) {
        return 0;
    }
    
    Int retval = 0;
    for (Int i = 0; i < MAX_SLOTS; ++i) {  // O(n)
        if ((m_playerVotes[slot][i].vote == TRUE) && 
            (m_playerVotes[slot][i].frame == TheGameLogic->getFrame())) {
            ++retval;
        }
    }
    
    return retval;
}
```

**Llamado Desde:**
```
isPlayerVotedOut() (L727)
  └─> countVotesForPlayer() (L733)
      └─> isPlayerInGame() (L757)
          └─> Llamado para cada jugador en update
```

**Cascada:**
```
Por cada jugador (8 × O(n)) = O(n²)
```

### Hallazgo 3.4: Chequeo de Frames Sincronizados - O(n)
**Línea:** 645-665
**Severidad:** MEDIA

```cpp
Bool DisconnectManager::allOnSameFrame(ConnectionManager *conMgr) {
    Bool retval = TRUE;
    for (Int i = 0; (i < MAX_SLOTS) && (retval == TRUE); ++i) {  // O(n)
        Int transSlot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
        if (transSlot == -1) {
            continue;
        }
        if ((conMgr->isPlayerConnected(i) == TRUE) && 
            (isPlayerInGame(transSlot, conMgr) == TRUE)) {
            
            if (m_disconnectFramesReceived[i] == FALSE) {
                retval = FALSE;
            }
            if ((m_disconnectFramesReceived[i] == TRUE) && 
                (m_disconnectFrames[conMgr->getLocalPlayerID()] != 
                 m_disconnectFrames[i])) {
                retval = FALSE;
            }
        }
    }
    return retval;
}
```

**Impacto:**
- Se llama durante la pantalla de desconexión
- Chequea si todos están en el mismo frame
- O(n) operación

### Hallazgo 3.5: Espera en Pantalla de Desconexión
**Línea:** 102-121
**Severidad:** ALTA

```cpp
void DisconnectManager::update(ConnectionManager *conMgr) {
    if (m_lastFrameTime == -1) {
        m_lastFrameTime = timeGetTime();
    }
    
    // CONDICIÓN DE BLOQUEO: Si no hemos avanzado de frame
    if (TheGameLogic->getFrame() == m_lastFrame) {  // L110
        time_t curTime = timeGetTime();
        if ((curTime - m_lastFrameTime) > 
            TheGlobalData->m_networkDisconnectTime) {  // Timeout
            
            if (m_disconnectState == DISCONNECTSTATETYPE_SCREENOFF) {
                turnOnScreen(conMgr);  // MUESTRA pantalla de desconexión
            }
            sendKeepAlive(conMgr);  // Mantiene conexión
        }
    } else {
        nextFrame(TheGameLogic->getFrame(), conMgr);  // Avanzamos
    }
}
```

**Implicación:**
- Si TheGameLogic::getFrame() no avanza
- Y pasa más de m_networkDisconnectTime (por defecto: varios segundos)
- MUESTRA PANTALLA DE DESCONEXIÓN
- Esto es lo que ven los jugadores cuando hay lag

---

## ARCHIVO 4: GameLogic.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp`

### Hallazgo 4.1: Punto de Control - isFrameDataReady()
**Línea:** 3046-3292
**Severidad:** CRÍTICA

```cpp
void GameLogic::update( void ) {
    // Este método SOLO se ejecuta si GameEngine.cpp línea 594 autoriza
    // Es decir, solo si TheNetwork->isFrameDataReady() == TRUE
}
```

**Verificación en GameEngine.cpp línea 594:**
```cpp
if ((TheNetwork == NULL && !TheGameLogic->isGamePaused()) || 
    (TheNetwork && TheNetwork->isFrameDataReady())) {
    TheGameLogic->UPDATE();  // Solo si isFrameDataReady() == TRUE
}
```

### Hallazgo 4.2: CRC Calculation per 100 Frames
**Línea:** 3119-3147
**Severidad:** MEDIA

```cpp
Bool isMPGameOrReplay = (TheRecorder && TheRecorder->isMultiplayer() && 
                         getGameMode() != GAME_SHELL && getGameMode() != GAME_NONE);

Bool generateForMP = (isMPGameOrReplay && 
                      (m_frame % TheGameInfo->getCRCInterval()) == 0);  // L3121

if (generateForMP) {
    m_CRC = getCRC( CRC_RECALC );  // OPERACIÓN COSTOSA
    
    GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_LOGIC_CRC );
    msg->appendIntegerArgument( m_CRC );
    msg->appendBooleanArgument( ... );
}
```

**Tabla de CRC Interval:**
```
DEBUG:   NET_CRC_INTERVAL = 1   (CADA FRAME)
RELEASE: NET_CRC_INTERVAL = 100 (cada 100 frames)
```

**Con 30 fps:**
```
- DEBUG: getCRC() cada 33ms (EXTREMADAMENTE LENTO)
- RELEASE: getCRC() cada 3.3 segundos
```

### Hallazgo 4.3: Script Engine Update
**Línea:** 3091-3093
**Severidad:** MEDIA

```cpp
TheScriptEngine->UPDATE();
```

**Costo:** O(# de scripts activos)
**Impacto:** Si hay scripts lentos, ralentiza el frame

### Hallazgo 4.4: Actualización de Objetos (Sleepy)
**Línea:** 3191-3231
**Severidad:** MEDIA

```cpp
while (!m_sleepyUpdates.empty()) {
    UpdateModulePtr u = peekSleepyUpdate();
    
    if (!u) {
        DEBUG_CRASH(("Null update. should not happen."));
        continue;
    }
    
    if (u->friend_getNextCallFrame() > now) {
        break;  // Salir del loop si nadie más necesita update
    }
    
    // Actualizar módulo
    sleepLen = u->update();
    u->friend_setNextCallFrame(now + sleepLen);
    rebalanceSleepyUpdate(0);
}
```

**Costo:** O(# de objetos que necesitan update en este frame)
**Impacto:** Varía según cantidad de unidades y actividad

### Hallazgo 4.5: AI System Update
**Línea:** 3236-3238
**Severidad:** MEDIA

```cpp
TheAI->UPDATE();  // Pathfinding, comportamiento
```

**Costo:** O(# de unidades IA × complejidad de IA)
**Impacto:** Significativo en partidas con muchas unidades IA

### Hallazgo 4.6: Partition Manager Update
**Línea:** 3246-3248
**Severidad:** MEDIA

```cpp
ThePartitionManager->UPDATE();
```

**Costo:** O(# de células × densidad de objetos)
**Impacto:** Mantiene grid espacial actualizado

### Hallazgo 4.7: Frame Increment
**Línea:** 3288-3291
**Severidad:** ALTA

```cpp
if (!m_startNewGame) {
    m_frame++;  // AVANZA SOLO SI ESTE MÉTODO SE EJECUTÓ
}
```

**Implicación:**
- Si GameLogic::update() no se ejecuta (porque AllCommandsReady() == FALSE)
- m_frame NO avanza
- DisconnectManager::update() lo detecta (compara con m_lastFrame)
- Si dura > m_networkDisconnectTime: MUESTRA PANTALLA DE DESCONEXIÓN

---

## ARCHIVO 5: FrameData.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameNetwork/FrameData.cpp`

### Hallazgo 5.1: Check de Comandos Listos
**Línea:** 107-136
**Severidad:** CRÍTICA

```cpp
FrameDataReturnType FrameData::allCommandsReady(Bool debugSpewage) {
    // Compara: # de comandos esperados vs # de comandos recibidos
    
    if (m_frameCommandCount == m_commandCount) {  // L108
        m_lastFailedFrameCC = -2;
        m_lastFailedCC = -2;
        return FRAMEDATA_READY;  // ¡VERDADERO!
    }
    
    if (m_commandCount > m_frameCommandCount) {  // L122
        // Demasiados comandos (error de sincronización)
        DEBUG_LOG(("FrameData::allCommandsReady - There are more commands "
                   "than there should be (%d, should be %d).  Commands in "
                   "command list are...\n", m_commandCount, m_frameCommandCount));
        
        NetCommandRef *ref = m_commandList->getFirstMessage();
        while (ref != NULL) {
            DEBUG_LOG(("%s, frame = %d, id = %d\n", 
                      GetAsciiNetCommandType(ref->getCommand()->getNetCommandType()).str(), 
                      ref->getCommand()->getExecutionFrame(), 
                      ref->getCommand()->getID()));
            ref = ref->getNext();
        }
        
        reset();  // Limpia la lista
        return FRAMEDATA_RESEND;  // PIDE RESEND
    }
    
    return FRAMEDATA_NOTREADY;  // Línea 135: ESPERANDO
}
```

**Estados Posibles:**
```
FRAMEDATA_READY    = Todos los comandos llegaron
FRAMEDATA_NOTREADY = Faltan comandos (BLOQUEA)
FRAMEDATA_RESEND   = Demasiados comandos (corrupto, pide resend)
```

### Hallazgo 5.2: Agregar Comando
**Línea:** 163-178
**Severidad:** MEDIA

```cpp
void FrameData::addCommand(NetCommandMsg *msg) {
    if (m_commandList == NULL) {
        init();
    }
    
    // Previene duplicados
    if (m_commandList->findMessage(msg) != NULL) {
        return;
    }
    
    m_commandList->addMessage(msg);
    ++m_commandCount;  // INCREMENTA
}
```

**Lógica:**
- Cada comando recibido incrementa m_commandCount
- Cuando m_commandCount == m_frameCommandCount: READY

---

## ARCHIVO 6: NetworkUtil.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameNetwork/NetworkUtil.cpp`

### Hallazgo 6.1: Constantes de Red
**Línea:** 30-33
**Severidad:** CRÍTICA

```cpp
Int MAX_FRAMES_AHEAD = 128;          // Línea 30
Int MIN_RUNAHEAD = 10;                // Línea 31
Int FRAME_DATA_LENGTH = (MAX_FRAMES_AHEAD+1)*2;  // = 258 (L32)
Int FRAMES_TO_KEEP = (MAX_FRAMES_AHEAD/2) + 1;   // = 65 (L33)
```

**Impacto:**
```
MAX_FRAMES_AHEAD = 128 frames
  - Con 30 fps = 128/30 = 4.27 segundos de buffer máximo
  - Si latencia > 4.27s: buffer overflow

MIN_RUNAHEAD = 10 frames
  - 10/30 = 333ms mínimo de adelanto

FRAME_DATA_LENGTH = 258
  - Doble buffer para almacenar frames
  - Circular buffer de comando history

FRAMES_TO_KEEP = 65
  - Si un jugador se desconecta, mantiene últimos 65 frames
  - Para resincronizar otros jugadores
```

**Tabla de Impacto por Latencia:**
```
Latencia     │ Run-Ahead Calculado │ Buffer Suficiente
─────────────┼────────────────────┼───────────────────
100ms        │ 1.5 frames         │ SÍ
200ms        │ 3 frames           │ SÍ
400ms        │ 6 frames           │ SÍ
800ms        │ 12 frames          │ SÍ
1600ms       │ 24 frames          │ SÍ (pero cerca del límite)
3200ms       │ 48 frames          │ PROBLEMA (muy cerca de 64)
4270ms+      │ >64 frames (capped) │ BLOQUEA/PIERDE DATOS
```

---

## ARCHIVO 7: FrameMetrics.cpp
**Ruta:** `/home/user/CnC_Generals_Zero_Hour-prueba/Generals/Code/GameEngine/Source/GameNetwork/FrameMetrics.cpp`

### Hallazgo 7.1: Almacenamiento de Latencias
**Línea:** 44-46
**Severidad:** MEDIA

```cpp
m_pendingLatencies = NEW time_t[MAX_FRAMES_AHEAD];  // Array de 128
for(Int i = 0; i < MAX_FRAMES_AHEAD; i++) {
    m_pendingLatencies[i] = 0;
}
```

**Memoria:** 128 × sizeof(time_t) = ~512 bytes por cliente

### Hallazgo 7.2: Cálculo de Latencia Promedio
**Línea:** 111-124
**Severidad:** MEDIA

```cpp
void FrameMetrics::processLatencyResponse(UnsignedInt frame) {
    time_t curTime = timeGetTime();
    Int pendingIndex = frame % MAX_FRAMES_AHEAD;  // Circular buffer
    time_t timeDiff = curTime - m_pendingLatencies[pendingIndex];
    
    Int latencyListIndex = frame % TheGlobalData->m_networkLatencyHistoryLength;
    m_averageLatency -= m_latencyList[latencyListIndex] / 
                        TheGlobalData->m_networkLatencyHistoryLength;
    m_latencyList[latencyListIndex] = (Real)timeDiff / (Real)1000;  // Convertir a segundos
    m_averageLatency += m_latencyList[latencyListIndex] / 
                        TheGlobalData->m_networkLatencyHistoryLength;
}
```

**Algoritmo:**
```
Average latency = suma de latencias recientes / # de muestras
Se actualiza circular cada nuevo ping-pong
```

### Hallazgo 7.3: Minimum Cushion Tracking
**Línea:** 126-135
**Severidad:** ALTA

```cpp
void FrameMetrics::addCushion(Int cushion) {
    ++m_cushionIndex;
    m_cushionIndex %= TheGlobalData->m_networkCushionHistoryLength;
    
    if (m_cushionIndex == 0) {
        m_minimumCushion = -1;  // Reset cuando ciclo completa
    }
    
    if ((cushion < m_minimumCushion) || (m_minimumCushion == -1)) {
        m_minimumCushion = cushion;  // Guarda el mínimo
    }
}
```

**Impacto:**
- Calcula el "cushion" = frames de adelanto disponibles
- Si cushion < runAheadPercentage × runAhead:
  - El juego se ralentiza a sí mismo ("self slug")
  - Se envía comando updateRunAhead() a todos

---

## RESUMEN DE CUELLOS DE BOTELLA CRÍTICOS

| Archivo | Línea | Función | Severidad | Tipo |
|---------|-------|---------|-----------|------|
| Network.cpp | 716 | AllCommandsReady() | CRÍTICA | Bloqueo |
| Network.cpp | 807 | isFrameDataReady() | CRÍTICA | Control |
| ConnectionManager.cpp | 1556 | allCommandsReady() | CRÍTICA | Loop O(n) |
| ConnectionManager.cpp | 1249 | updateRunAhead | MEDIA | Cálculo |
| DisconnectManager.cpp | 66 | m_playerVotes init | ALTA | O(n²) |
| DisconnectManager.cpp | 782 | countVotesForPlayer | MEDIA | Loop O(n) |
| GameLogic.cpp | 3132 | getCRC() | MEDIA | Costo O(m) |
| GameEngine.cpp | 594 | isFrameDataReady check | CRÍTICA | Condición |
| FrameData.cpp | 107 | allCommandsReady | CRÍTICA | Comparación |

---

## TABLA DE ESCALABILIDAD CON # JUGADORES

| Jugadores | allCommandsReady | DisconnectMgr | countVotes | Votos Total |
|-----------|-----------------|---------------|------------|-------------|
| 1 | O(1) | O(1) | N/A | 0 |
| 2 | O(2) | O(2) | O(2) | 4 |
| 4 | O(4) | O(4) | O(4) | 16 |
| 6 | O(6) | O(6) | O(6) | 36 |
| 8 | O(8) | O(8) | O(8) | 64 |
| 16 | O(16) | O(16) | O(16) | 256 |

**Observación:** Si MAX_SLOTS aumentara a 16, los votos pasarían de O(64) a O(256).

