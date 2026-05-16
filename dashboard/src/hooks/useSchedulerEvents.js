import { useState, useEffect, useRef, useCallback } from 'react';

const WS_URL = 'ws://localhost:8080';

// Palette of visually distinct colors for processes. Avoids gray (TERMINATED)
// and red (reserved for BLOCKED).
const PROCESS_COLORS = [
  '#22c55e', // green
  '#3b82f6', // blue
  '#a855f7', // purple
  '#f97316', // orange
  '#ec4899', // pink
  '#06b6d4', // cyan
  '#eab308', // yellow
  '#14b8a6', // teal
  '#f43f5e', // rose
  '#8b5cf6', // violet
];

export default function useSchedulerEvents() {
  const [processes, setProcesses] = useState({});
  const [events, setEvents] = useState([]);
  const [connected, setConnected] = useState(false);
  const [currentSlice, setCurrentSlice] = useState(500);
  const [isCapturing, setIsCapturing] = useState(true);
  const startTimeRef = useRef(null);
  const colorIndexRef = useRef(0);
  const isCapturingRef = useRef(true);

  // Time-freezing: while paused, the "clock" stops advancing.
  // pausedAtRef = timestamp (ms) when pause started, null if capturing.
  // totalPausedMsRef = accumulated pause duration across all pauses.
  const pausedAtRef = useRef(null);
  const totalPausedMsRef = useRef(0);

  // Keep the ref in sync with state so the WebSocket handler can read it
  useEffect(() => {
    isCapturingRef.current = isCapturing;
  }, [isCapturing]);

  const getRelativeTime = useCallback(() => {
    if (!startTimeRef.current) return 0;
    // If currently paused, freeze time at the moment pause started
    const refNow = pausedAtRef.current ?? Date.now();
    return (refNow - startTimeRef.current - totalPausedMsRef.current) / 1000;
  }, []);

  const startCapture = useCallback(() => {
    // If we were paused, accumulate the paused duration
    if (pausedAtRef.current !== null) {
      totalPausedMsRef.current += Date.now() - pausedAtRef.current;
      pausedAtRef.current = null;
    }
    setIsCapturing(true);
    if (!startTimeRef.current) startTimeRef.current = Date.now();
  }, []);

  const stopCapture = useCallback(() => {
    if (pausedAtRef.current === null) {
      pausedAtRef.current = Date.now();
    }
    setIsCapturing(false);
  }, []);

  const resetCapture = useCallback(() => {
    setProcesses({});
    setEvents([]);
    startTimeRef.current = Date.now();
    colorIndexRef.current = 0;
    totalPausedMsRef.current = 0;
    // If currently paused, reset pause start to now so clock stays at 0
    if (pausedAtRef.current !== null) {
      pausedAtRef.current = Date.now();
    }
  }, []);

  useEffect(() => {
    let ws;
    let reconnectTimer;

    function connect() {
      ws = new WebSocket(WS_URL);

      ws.onopen = () => {
        setConnected(true);
        if (!startTimeRef.current) startTimeRef.current = Date.now();
      };

      ws.onclose = () => {
        setConnected(false);
        reconnectTimer = setTimeout(connect, 2000);
      };

      ws.onmessage = (msg) => {
        // Drop events while capture is paused
        if (!isCapturingRef.current) return;

        try {
          const event = JSON.parse(msg.data);
          setEvents(prev => [...prev.slice(-500), event]);

          switch (event.type) {
            case 'PROCESS_CREATED': {
              const color = PROCESS_COLORS[colorIndexRef.current % PROCESS_COLORS.length];
              colorIndexRef.current++;
              const createdAt = getRelativeTime();
              setProcesses(prev => {
                // Si ya hay procesos activos, este empieza en READY sin segmento abierto
                const hasRunning = Object.values(prev).some(p => p.state === 'RUNNING');
                return {
                  ...prev,
                  [event.pid]: {
                    pid: event.pid,
                    name: event.name,
                    state: hasRunning ? 'READY' : 'RUNNING',
                    cpuTime: 0,
                    switches: 0,
                    segments: hasRunning ? [] : [{ start: createdAt, end: null }],
                    createdAt,
                    color,
                  }
                };
              });
              break;
            }

            case 'CONTEXT_SWITCH':
              setProcesses(prev => {
                const now = getRelativeTime();
                const updated = { ...prev };

                if (updated[event.from]) {
                  const proc = { ...updated[event.from] };
                  const segs = [...proc.segments];
                  const lastSeg = segs[segs.length - 1];
                  if (lastSeg && !lastSeg.end) {
                    lastSeg.end = now;
                    // Acumular CPU del segmento que acaba de terminar
                    proc.cpuTime = (proc.cpuTime || 0) + (lastSeg.end - lastSeg.start) * 1000;
                  }
                  proc.segments = segs;
                  proc.state = 'READY';
                  proc.switches = (proc.switches || 0) + 1;
                  updated[event.from] = proc;
                }

                if (updated[event.to]) {
                  const proc = { ...updated[event.to] };
                  proc.segments = [...proc.segments, { start: now, end: null, state: 'RUNNING' }];
                  proc.state = 'RUNNING';
                  updated[event.to] = proc;
                }

                Object.keys(updated).forEach(pid => {
                const p = updated[pid];
                if (p.state === 'TERMINATED') {
                  const segs = [...p.segments];
                  const last = segs[segs.length - 1];
                  if (last && !last.end) {
                    last.end = now;
                    updated[pid] = { ...p, segments: segs };
                  }
                }
              });

                return updated;
              });
              setCurrentSlice(event.slice_ms);
              break;

            case 'PROCESS_TERMINATED':
              setProcesses(prev => {
                if (!prev[event.pid]) return prev;
                const now = getRelativeTime();
                const proc = { ...prev[event.pid] };
                const lastSeg = proc.segments[proc.segments.length - 1];
                if (lastSeg && !lastSeg.end) lastSeg.end = now;
                proc.state = 'TERMINATED';
                proc.cpuTime = event.cpu_ms;
                proc.switches = event.switches;
                return { ...prev, [event.pid]: proc };
              });
              break;

            case 'SLICE_CHANGED':
              setCurrentSlice(event.new_ms);
              break;
          }
        } catch (e) {
          console.error('Parse error:', e);
        }
      };
    }

    connect();

    return () => {
      clearTimeout(reconnectTimer);
      if (ws) ws.close();
    };
  }, [getRelativeTime]);

  return {
    processes,
    events,
    connected,
    currentSlice,
    isCapturing,
    getRelativeTime,
    startCapture,
    stopCapture,
    resetCapture,
  };
}
