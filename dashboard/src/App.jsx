import { useState } from 'react';
import useSchedulerEvents from './hooks/useSchedulerEvents';
import ProcessTable from './components/ProcessTable';
import GanttChart from './components/GanttChart';
import RegisterView from './components/RegisterView';
import MetricsPanel from './components/MetricsPanel';
import CaptureControls from './components/CaptureControls';

export default function App() {
  const {
    processes,
    events,
    connected,
    currentSlice,
    isCapturing,
    getRelativeTime,
    startCapture,
    stopCapture,
    resetCapture,
  } = useSchedulerEvents();
  const [selectedPid, setSelectedPid] = useState(null);

  return (
    <div className="min-h-screen bg-gray-950 text-gray-200 p-4">
      {/* Header */}
      <div className="mb-4">
        <h1 className="text-2xl font-bold text-white">CamilOS Dashboard</h1>
        <p className="text-gray-500 text-sm">Simulador de Context Switching — Observabilidad en tiempo real</p>
      </div>

      {/* Capture controls bar (Instruments-style) */}
      <div className="mb-4">
        <CaptureControls
          isCapturing={isCapturing}
          onStart={startCapture}
          onStop={stopCapture}
          onReset={resetCapture}
          getRelativeTime={getRelativeTime}
        />
      </div>

      {/* Main grid: 2 columns */}
      <div className="grid grid-cols-1 lg:grid-cols-4 gap-4">

        {/* Left: Gantt + Process Table (3 cols) */}
        <div className="lg:col-span-3 space-y-4">

          {/* Gantt Chart */}
          <div className="bg-gray-900 rounded-xl border border-gray-800 p-4">
            <h2 className="text-sm font-semibold text-gray-400 mb-3">Gantt Chart — Uso de CPU</h2>
            <GanttChart processes={processes} getRelativeTime={getRelativeTime} />
          </div>

          {/* Process Table */}
          <div className="bg-gray-900 rounded-xl border border-gray-800 p-4">
            <h2 className="text-sm font-semibold text-gray-400 mb-3">Process Table</h2>
            <ProcessTable
              processes={processes}
              selectedPid={selectedPid}
              onSelect={setSelectedPid}
            />
          </div>
        </div>

        {/* Right sidebar (1 col) */}
        <div className="space-y-4">

          {/* Metrics */}
          <div className="bg-gray-900 rounded-xl border border-gray-800 p-4">
            <h2 className="text-sm font-semibold text-gray-400 mb-3">Metricas</h2>
            <MetricsPanel
              processes={processes}
              events={events}
              currentSlice={currentSlice}
              connected={connected}
            />
          </div>

          {/* Register View */}
          <div className="bg-gray-900 rounded-xl border border-gray-800 p-4">
            <h2 className="text-sm font-semibold text-gray-400 mb-3">Detalle del Proceso</h2>
            <RegisterView
              processes={processes}
              selectedPid={selectedPid}
              events={events}
            />
          </div>

          {/* Event Log */}
          <div className="bg-gray-900 rounded-xl border border-gray-800 p-4">
            <h2 className="text-sm font-semibold text-gray-400 mb-3">
              Eventos recientes ({events.length})
            </h2>
            <div className="max-h-48 overflow-y-auto space-y-1 text-xs font-mono">
              {events.slice(-20).reverse().map((e, i) => (
                <div key={i} className="text-gray-500 truncate">
                  <span className="text-gray-600">{e.type}</span>
                  {e.from && <span className="text-yellow-600"> {e.from}→{e.to}</span>}
                  {e.pid && !e.from && <span className="text-green-600"> PID {e.pid}</span>}
                  {e.name && <span className="text-blue-600"> {e.name}</span>}
                </div>
              ))}
              {events.length === 0 && (
                <div className="text-gray-600">
                  {isCapturing ? 'Esperando eventos...' : 'Captura pausada'}
                </div>
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
