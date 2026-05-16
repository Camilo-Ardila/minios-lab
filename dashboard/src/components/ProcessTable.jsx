const STATE_TEXT = {
  RUNNING: 'text-green-400',
  READY: 'text-yellow-400',
  BLOCKED: 'text-red-400',
  TERMINATED: 'text-gray-500',
  NEW: 'text-blue-400',
};

export default function ProcessTable({ processes, selectedPid, onSelect }) {
  const procs = Object.values(processes);

  if (procs.length === 0) {
    return (
      <div className="text-gray-500 text-center py-8">
        No hay procesos. Usa <code className="text-gray-400">run</code> en CamilOS para lanzar uno.
      </div>
    );
  }

  return (
    <table className="w-full text-sm">
      <thead>
        <tr className="text-gray-400 border-b border-gray-700">
          <th className="text-left py-2 px-2 w-6"></th>
          <th className="text-left py-2 px-2">PID</th>
          <th className="text-left py-2 px-2">Nombre</th>
          <th className="text-left py-2 px-2">Estado</th>
          <th className="text-right py-2 px-2">CPU (ms)</th>
          <th className="text-right py-2 px-2">Switches</th>
        </tr>
      </thead>
      <tbody>
        {procs.map(proc => (
          <tr
            key={proc.pid}
            className={`border-b border-gray-800 cursor-pointer hover:bg-gray-800 transition-colors ${
              selectedPid === proc.pid ? 'bg-gray-800' : ''
            }`}
            onClick={() => onSelect(proc.pid)}
          >
            {/* Color swatch identifying the process */}
            <td className="py-2 px-2">
              <span
                className="inline-block w-3 h-3 rounded"
                style={{ backgroundColor: proc.color || '#6b7280' }}
              ></span>
            </td>
            <td className="py-2 px-2 font-mono">{proc.pid}</td>
            <td className="py-2 px-2">{proc.name}</td>
            <td className="py-2 px-2">
              <span className={STATE_TEXT[proc.state] || 'text-gray-400'}>
                {proc.state}
              </span>
            </td>
            <td className="py-2 px-2 text-right font-mono">{(proc.cpuTime || 0).toFixed(1)}</td>
            <td className="py-2 px-2 text-right font-mono">{proc.switches || 0}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
