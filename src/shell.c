/*
 * shell.c — SHELL INTERACTIVO DE miniOS
 *
 * Este archivo implementa el prompt `miniOS>` y el parser de comandos.
 * El main loop (shell_run) y varios comandos (help, slice, inspect, runpair,
 * log) vienen ya implementados.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. cmd_run         — validar path y lanzar proceso con el scheduler
 *   2. cmd_ps          — mostrar la process table (process status)
 *   3. cmd_kill_proc   — matar un proceso por PID
 *   4. cmd_stats       — mostrar metricas agregadas del scheduler
 *
 * Los comandos implementados (cmd_help, cmd_slice, cmd_inspect, cmd_runpair)
 * sirven como referencia de estilo, convenciones y manejo de argumentos.
 *
 * REGLAS IMPORTANTES:
 *   - Cualquier comando que LEA process_table o ready_queue debe envolverse
 *     en block_alarm() / unblock_alarm() para evitar race con scheduler_tick.
 *   - Usa las funciones auxiliares ya disponibles:
 *       pcb_print_table()  en pcb.h
 *       pcb_state_name()   en pcb.h
 *       rq_print()         en ready_queue.h
 *       rq_remove()        en ready_queue.h
 *       timer_get_slice()  en timer.h
 */

#include "shell.h"
#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "pcb.h"
#include "ready_queue.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINE 256

// ============================================================
// Helpers y comandos ya implementados — NO los modifiques
// ============================================================

// Block SIGALRM while modifying shared state
static sigset_t alarm_mask;

static void block_alarm(void) {
    sigprocmask(SIG_BLOCK, &alarm_mask, NULL);
}

static void unblock_alarm(void) {
    sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

static void cmd_help(void) {
    printf("\nComandos disponibles:\n");
    printf("  run <binario>       Lanzar un proceso nuevo\n");
    printf("  ps                  Mostrar tabla de procesos\n");
    printf("  kill <pid>          Terminar un proceso\n");
    printf("  slice <ms>          Cambiar time slice (50-5000 ms)\n");
    printf("  inspect <pid>       Ver registros de un proceso\n");
    printf("  runpair <nombre>    Lanzar par de procesos comunicantes\n");
    printf("  stats               Mostrar metricas del scheduler\n");
    printf("  log on|off          Activar/desactivar emision de eventos JSON\n");
    printf("  help                Mostrar esta ayuda\n");
    printf("  exit                Terminar todos los procesos y salir\n");
    printf("\n");
}

static void cmd_slice(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Time slice actual: %d ms\n", timer_get_slice());
        return;
    }

    int ms = atoi(arg);
    if (ms < 50 || ms > 5000) {
        printf("Error: time slice debe estar entre 50 y 5000 ms\n");
        return;
    }

    int old_ms = timer_get_slice();
    timer_set_slice(ms);
    if (scheduler_is_running()) {
        timer_start();
    }
    monitor_emit_slice_changed(old_ms, ms);
    printf("Time slice cambiado a %d ms\n", ms);
}

static void cmd_inspect(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Uso: inspect <pid>\n");
        return;
    }

    int target_pid = atoi(arg);
    block_alarm();

    int found = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == target_pid) {
            found = 1;
            printf("\n=== PCB de PID %d (%s) ===\n", target_pid, process_table[i].name);
            printf("  Estado:           %s\n", pcb_state_name(process_table[i].state));
            printf("  CPU Time:         %.1f ms\n", process_table[i].cpu_time_ms);
            printf("  Waiting Time:     %.1f ms\n", process_table[i].wait_time_ms);
            printf("  Context Switches: %d\n", process_table[i].context_switches);

            if (platform_registers_available() && process_table[i].regs_valid) {
                printf("\n  === Registros ===\n");
                printf("  Program Counter: 0x%016llx\n", (unsigned long long)process_table[i].registers.program_counter);
                printf("  Stack Pointer:   0x%016llx\n", (unsigned long long)process_table[i].registers.stack_pointer);
                for (int r = 0; r < NUM_GENERAL_REGS; r++) {
                    printf("  Reg[%2d]:         0x%016llx\n", r,
                           (unsigned long long)process_table[i].registers.general_regs[r]);
                }
            } else if (!platform_registers_available()) {
                printf("\n  [Registros no disponibles -- SIP habilitado en macOS]\n");
                printf("  Los registros estaran disponibles en WSL2/Linux.\n");
            } else {
                printf("\n  [Registros aun no capturados]\n");
            }
            printf("\n");
            break;
        }
    }

    if (!found) {
        printf("Proceso PID %d no encontrado.\n", target_pid);
    }

    unblock_alarm();
}

static void cmd_runpair(const char *name) {
    if (!name || strlen(name) == 0) {
        printf("Uso: runpair <nombre>\n");
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    char server_path[128], client_path[128], sock_path[128];

    if (strcmp(name, "ping_pong") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/ping_pong_server");
        snprintf(client_path, sizeof(client_path), "programs/bin/ping_pong_client");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_pingpong.sock");
    } else if (strcmp(name, "productor_consumidor") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/productor");
        snprintf(client_path, sizeof(client_path), "programs/bin/consumidor");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_prodcons.sock");
    } else {
        printf("Par desconocido: '%s'\n", name);
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    if (access(server_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", server_path);
        return;
    }
    if (access(client_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", client_path);
        return;
    }

    unlink(sock_path);
    printf("Lanzando par '%s' con socket %s\n", name, sock_path);

    int idx1 = scheduler_create_process(server_path, sock_path);
    if (idx1 < 0) return;

    int idx2 = scheduler_create_process(client_path, sock_path);
    if (idx2 < 0) return;

    if (!scheduler_is_running()) {
        scheduler_start(timer_get_slice());
    }
}


// ============================================================
// [TODO 1/4] cmd_run
// ------------------------------------------------------------
// Parsea el comando `run <path> [arg]`, valida la ruta y crea
// un proceso a traves del scheduler. Si es el primer proceso,
// arranca el scheduler con el slice actual.
//
// Ejemplo de uso:
//   miniOS> run programs/bin/countdown 10
// ============================================================
static void cmd_run(const char *path, const char *arg) {
    // Paso 1. Si path es NULL o vacio, imprimir mensaje de uso y retornar:
    //         "Uso: run <binario> [argumento]"
    if (!path || strlen(path) == 0) {
        printf("[CamilOS] ¿Qué quieres correr, manito? Usa: run <binario> [argumento]\n");
        return;
    }

    // Paso 2. Verificar que el binario existe y es ejecutable con access(path, X_OK).
    //         Si no: imprimir error y retornar.
    if (access(path, X_OK) != 0) {
        printf("[CamilOS] '%s' no existe o no es ejecutable, revisa el path\n", path);
        return;
    }

    // Paso 3. Bloquear SIGALRM con block_alarm() antes de modificar
    //         process_table (evitar race condition con scheduler_tick).
    block_alarm();

    // Paso 4. Llamar scheduler_create_process(path, arg).
    //         Si retorna -1, imprimir error, desbloquear y retornar.
    int idx = scheduler_create_process(path, arg);
    if (idx < 0) {
        printf("[CamilOS] no se pudo crear el proceso, algo falló en el scheduler\n");
        unblock_alarm();
        return;
    }

    // Paso 5. Si el scheduler no está corriendo aún (!scheduler_is_running()),
    //         llamar scheduler_start(timer_get_slice()) para arrancar.
    if (!scheduler_is_running()) {
        scheduler_start(timer_get_slice());
    }

    // Paso 6. Desbloquear SIGALRM con unblock_alarm().
    unblock_alarm();
}


// ============================================================
// [TODO 2/4] cmd_ps
// ------------------------------------------------------------
// Muestra la process table completa y el estado de la ready
// queue. Equivalente al `ps` de un SO real pero dentro de
// CamilOS.
//
// Columnas: PID | Nombre | Estado | CPU (ms) | Espera (ms) | Switches
// ============================================================
static void cmd_ps(void) {
    // Paso 1. Bloquear SIGALRM — vamos a leer process_table y ready queue,
    //         que también modifica scheduler_tick. Sin este bloqueo podría
    //         haber una race condition y leer datos inconsistentes.
    block_alarm();

    // Paso 2. Si no hay procesos todavía, avisar y salir.
    if (process_count == 0) {
        printf("[CamilOS] no hay procesos, usa 'run <binario>' para crear uno\n");
        unblock_alarm();
        return;
    }

    // Paso 3. Imprimir encabezado de la tabla.
    printf("\n");
    printf("╔═════════════════════════════════════════════════════════════════════╗\n");
    printf("║                  La gran tabla de procesos de CamilOS               ║\n");
    printf("╠═══════╦══════════════════╦═══════════╦══════════╦══════════╦════════╣\n");
    printf("║  PID  ║      Nombre      ║  Estado   ║ CPU (ms) ║ Wait(ms) ║ Switch ║\n");
    printf("╠═══════╬══════════════════╬═══════════╬══════════╬══════════╬════════╣\n");

    // Paso 4. Iterar process_table e imprimir cada entrada.
    for (int i = 0; i < process_count; i++) {
        pcb_t *p = &process_table[i];
        printf("║ %5d ║ %-16s ║ %-9s ║ %8.1f ║ %8.1f ║ %6d ║\n",
            p->pid,
            p->name,
            pcb_state_name(p->state),
            p->cpu_time_ms,
            p->wait_time_ms,
            p->context_switches);
    }

    printf("╚═══════╩══════════════════╩═══════════╩══════════╩══════════╩════════╝\n");

    // Paso 5. Imprimir la ready queue con rq_print().
    printf("\nReady Queue (En orden de ejecución):\n");
    rq_print();

    // Paso 6. Imprimir slice actual como referencia.
    printf("\nTime slice actual: %d ms\n\n", timer_get_slice());

    // Paso 7. Desbloquear SIGALRM.
    unblock_alarm();
}


// ============================================================
// [TODO 3/4] cmd_kill_proc
// ------------------------------------------------------------
// Termina un proceso por PID: le manda SIGKILL, lo remueve de
// la ready queue y marca su PCB como PROC_TERMINATED.
//
// Ejemplo de uso:
//   CamilOS> kill 1234
// ============================================================
static void cmd_kill_proc(const char *arg) {
    // Paso 1. Si arg es NULL o vacío, imprimir uso y retornar.
    if (!arg || strlen(arg) == 0) {
        printf("[CamilOS] ¿A quién nos bajamos? Usa: kill <pid>\n");
        return;
    }

    // Paso 2. Convertir arg a entero con atoi().
    int target_pid = atoi(arg);
    if (target_pid <= 0) {
        printf("[CamilOS] 🚫 PID inválido: '%s'\n", arg);
        return;
    }

    // Paso 3. Bloquear SIGALRM — vamos a modificar process_table y ready queue.
    block_alarm();

    // Paso 4. Buscar el PID en process_table.
    int found = -1;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == target_pid) {
            found = i;
            break;
        }
    }

    // Paso 5. Si no se encontró o ya está terminado, avisar y salir.
    if (found < 0) {
        printf("[CamilOS] PID %d no encontrado, mi rey\n", target_pid);
        unblock_alarm();
        return;
    }
    if (process_table[found].state == PROC_TERMINATED) {
        printf("[CamilOS] PID %d ya ha terminado, mi rey\n", target_pid);
        unblock_alarm();
        return;
    }

    // Paso 6. Mandar SIGKILL al proceso.
    kill(target_pid, SIGKILL);
    printf("[CamilOS] PID %d (%s) eliminado , que descanse en paz\n",
           target_pid, process_table[found].name);

    // Paso 7. Si era el proceso RUNNING, limpiar current_running.
    //         scheduler_sigchld lo despachará al siguiente automáticamente.
    if (found == scheduler_get_running()) {
        printf("[CamilOS] Era el proceso activo, despachando al siguiente...\n");
    }

    // Paso 8. Si estaba en la ready queue (estado READY), removerlo.
    if (process_table[found].state == PROC_READY) {
        rq_remove(found);
        process_table[found].state = PROC_TERMINATED;
    }

    // Paso 9. Desbloquear SIGALRM.
    unblock_alarm();
}


// ============================================================
// [TODO 4/4] cmd_stats
// ------------------------------------------------------------
// Calcula y muestra métricas agregadas del scheduler:
// CPU total, switches totales, procesos activos/terminados,
// promedios de CPU y switches por proceso.
//
// Ejemplo de uso:
//   CamilOS> stats
// ============================================================
static void cmd_stats(void) {
    // Paso 1. Bloquear SIGALRM — leemos process_table completa.
    block_alarm();

    // Paso 2. Si no hay procesos, avisar y salir.
    if (process_count == 0) {
        printf("[CamilOS] 📊 Sin datos aún — lanza procesos con 'run'\n");
        unblock_alarm();
        return;
    }

    // Paso 3. Calcular métricas iterando process_table.
    double total_cpu    = 0.0;
    int    total_sw     = 0;
    int    activos      = 0;
    int    terminados   = 0;
    double max_cpu      = 0.0;
    char   max_name[64] = "ninguno";

    for (int i = 0; i < process_count; i++) {
        pcb_t *p = &process_table[i];
        total_cpu += p->cpu_time_ms;
        total_sw  += p->context_switches;

        if (p->state != PROC_TERMINATED) {
            activos++;
        } else {
            terminados++;
        }

        if (p->cpu_time_ms > max_cpu) {
            max_cpu = p->cpu_time_ms;
            strncpy(max_name, p->name, sizeof(max_name) - 1);
        }
    }

    double avg_cpu = total_cpu / process_count;
    double avg_sw  = (double)total_sw / process_count;

    // Paso 4. Imprimir reporte.
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║     CamilOS:El maldito reporte de procesos   ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Procesos totales   : %-4d                   ║\n", process_count);
    printf("║  Activos            : %-4d                   ║\n", activos);
    printf("║  Terminados         : %-4d                   ║\n", terminados);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  CPU total          : %8.1f ms            ║\n", total_cpu);
    printf("║  CPU promedio       : %8.1f ms            ║\n", avg_cpu);
    printf("║  Mayor consumidor   : %-16s          ║\n", max_name);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Context switches   : %-6d                 ║\n", total_sw);
    printf("║  Switches promedio  : %8.1f               ║\n", avg_sw);
    printf("║  Time slice actual  : %-4d ms               ║\n", timer_get_slice());
    printf("╚══════════════════════════════════════════════╝\n");
    printf("\n");

    // Paso 5. Desbloquear SIGALRM.
    unblock_alarm();
}


// ============================================================
// Main loop del shell — ya implementado, NO lo modifiques
// ============================================================
void shell_run(void) {
    char line[MAX_LINE];

    // Setup alarm mask for sigprocmask
    sigemptyset(&alarm_mask);
    sigaddset(&alarm_mask, SIGALRM);

    printf("+----------------------------------+\n");
    printf("|       CamilOS v1.0               |\n");
    printf("|   Simulador de Context Switching |\n");
    printf("|   Escribe 'help' para ayuda      |\n");
    printf("+----------------------------------+\n\n");

    while (1) {
        printf("CamilOS> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strlen(line) == 0) continue;

        char *cmd = strtok(line, " \t");
        char *arg = strtok(NULL, " \t");
        char *arg2 = strtok(NULL, " \t");

        if (!cmd) continue;

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "run") == 0) {
            block_alarm();
            cmd_run(arg, arg2);
            unblock_alarm();
        } else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill_proc(arg);
        } else if (strcmp(cmd, "slice") == 0) {
            cmd_slice(arg);
        } else if (strcmp(cmd, "inspect") == 0) {
            cmd_inspect(arg);
        } else if (strcmp(cmd, "runpair") == 0) {
            block_alarm();
            cmd_runpair(arg);
            unblock_alarm();
        } else if (strcmp(cmd, "stats") == 0) {
            cmd_stats();
        } else if (strcmp(cmd, "log") == 0) {
            if (!arg || strlen(arg) == 0) {
                printf("Log esta %s. Uso: log on|off\n",
                       monitor_is_enabled() ? "activado" : "desactivado");
            } else if (strcmp(arg, "on") == 0) {
                monitor_init(MONITOR_SOCKET_PATH);
                monitor_set_enabled(1);
            } else if (strcmp(arg, "off") == 0) {
                monitor_set_enabled(0);
                printf("Log desactivado.\n");
            } else {
                printf("Uso: log on|off\n");
            }
        } else if (strcmp(cmd, "exit") == 0) {
            block_alarm();
            scheduler_stop();
            monitor_close();
            unblock_alarm();
            printf("Hasta luego, mi rey.\n");
            break;
        } else {
            printf("Comando desconocido: '%s'. Escribe 'help' para ver comandos.\n", cmd);
        }
    }
}
