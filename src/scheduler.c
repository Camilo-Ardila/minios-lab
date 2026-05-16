/*
 * scheduler.c — ESQUELETO DEL LABORATORIO
 *
 * Este archivo contiene el núcleo del scheduler round-robin de miniOS.
 * Las funciones de infraestructura (init, getters, install_sigchld, stop,
 * timespec_diff_ms) ya están implementadas.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. scheduler_create_process  — fork + exec + SIGSTOP + PCB init
 *   2. scheduler_start           — arrancar el primer proceso y el timer
 *   3. scheduler_tick            — handler de SIGALRM (context switch)
 *   4. scheduler_sigchld         — handler de SIGCHLD (terminación)
 *
 * Cada función viene con comentarios numerados que describen el flujo
 * paso a paso. Tu trabajo es traducir cada paso a código C usando las
 * APIs de POSIX y las funciones de infraestructura disponibles.
 *
 * APIs disponibles:
 *   - POSIX:       fork, execl, waitpid, kill, clock_gettime
 *   - platform_*:  ver src/platform/platform.h
 *   - pcb_*:       ver src/pcb.h
 *   - rq_*:        ver src/ready_queue.h
 *   - timer_*:     ver src/timer.h
 *   - monitor_*:   ver src/monitor.h
 *
 * REGLAS DE SEGURIDAD EN SEÑALES (importantes para scheduler_tick y
 * scheduler_sigchld):
 *   - NO uses printf/fprintf dentro de los handlers (no son
 *     async-signal-safe). Solo kill, waitpid, clock_gettime, write.
 *   - El shell bloquea SIGALRM con sigprocmask durante sus operaciones
 *     críticas, por lo que no necesitas mutex manual sobre process_table.
 */

#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>

// Estado global del scheduler
static volatile int current_running = -1;   // índice en process_table del proceso RUNNING, -1 si ninguno
static volatile int scheduler_active = 0;   // 1 si el scheduler está corriendo

// ============================================================
// Helpers ya implementados — NO los modifiques
// ============================================================

double timespec_diff_ms(struct timespec end, struct timespec start) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}

void scheduler_init(void) {
    process_count = 0;
    current_running = -1;
    scheduler_active = 0;
    rq_init();
}

int scheduler_get_running(void) {
    return current_running;
}

int scheduler_is_running(void) {
    return scheduler_active;
}

void scheduler_install_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

void scheduler_stop(void) {
    timer_stop();
    scheduler_active = 0;

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_TERMINATED) {
            kill(process_table[i].pid, SIGKILL);
            int status;
            waitpid(process_table[i].pid, &status, 0);
            process_table[i].state = PROC_TERMINATED;
        }
    }
    current_running = -1;
}


// ============================================================
// [TODO 1/4] scheduler_create_process
// ------------------------------------------------------------
// Crea un proceso nuevo a partir de un binario, lo deja detenido
// con estado PROC_READY y lo encola en la ready queue.
//
// Retorna el índice del nuevo PCB en process_table, o -1 en error.
//
// Observable correcto: `ps aux | grep <binario>` debe mostrar el
// proceso en estado T (stopped) justo después de crearlo.
// ============================================================
int scheduler_create_process(const char *path, const char *arg) {
    // Paso 1. Validar que hay espacio en process_table.
    //         Si process_count >= MAX_PROCESSES, imprimir error y retornar -1.
    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "[CamilOS] la Sala de espera está llena, no hay cama pa' tanta gente (max %d)\n", MAX_PROCESSES);
        return -1;
    }

    // Paso 2. Llamar fork() y guardar el resultado en una variable pid_t.
    //         Si fork() retorna < 0, es error: perror("fork") y retornar -1.
    pid_t pid = fork();
    if (pid < 0) {
        perror("[CamilOS] fork fallido :(  )");
        return -1;
    }

    // Paso 3. Si estamos en el HIJO (pid == 0):
    //         a) Si platform_uses_ptrace() retorna verdadero, llamar platform_trace_child().
    //            (En macOS esto es no-op; en Linux habilita ptrace.)
    //         b) Llamar execl(path, path, arg, NULL) si arg != NULL,
    //            o execl(path, path, NULL) si arg == NULL.
    //         c) Si execl retorna, falló: perror("execl") y _exit(1).
    if (pid == 0) {
        if (platform_uses_ptrace()) {
            platform_trace_child();
        }
        if (arg != NULL) {
            execl(path, path, arg, NULL);
        } else {
            execl(path, path, NULL);
        }
        perror("[CamilOS] execl fallido :( ");
        _exit(1);
    }

    // Paso 4. Si estamos en el PADRE (pid > 0) Y platform_uses_ptrace() es verdadero:
    //         a) waitpid(pid, &status, 0) para esperar el SIGTRAP post-exec.
    //         b) Verificar WIFSTOPPED(status). Si no está detenido: matar al hijo y retornar -1.
    if (platform_uses_ptrace()) {
        int status;
        waitpid(pid, &status, 0);
        if (!WIFSTOPPED(status)) {
            fprintf(stderr, "[CamilOS] El hijo no se detuvo tras exec, abortando\n");
            kill(pid, SIGKILL);
            return -1;
        }
    }

    // Paso 5. Crear la entrada en el PCB:
    //         - Calcular idx = process_count (índice libre en la tabla)
    //         - Obtener nombre corto con basename() sobre una copia de path
    //         - Llamar pcb_init(&process_table[idx], pid, short_name)
    //         - Liberar la copia del path
    //
    //         Ejemplo:
    //             char *path_copy = strdup(path);
    //             char *short_name = basename(path_copy);
    //             pcb_init(&process_table[idx], pid, short_name);
    //             free(path_copy);
    int idx = process_count;
    char *path_copy = strdup(path);
    char *short_name = basename(path_copy);
    pcb_init(&process_table[idx], pid, short_name);
    free(path_copy);

    // Paso 6. (Solo si platform_uses_ptrace()) Intentar capturar registros iniciales:
    //         a) Si platform_get_registers(pid, &process_table[idx].registers) retorna 0,
    //            marcar process_table[idx].regs_valid = 1.
    //         b) Llamar platform_detach(pid) para liberar el tracing.
    if (platform_uses_ptrace()) {
        if (platform_get_registers(pid, &process_table[idx].registers) == 0) {
            process_table[idx].regs_valid = 1;
        }
        platform_detach(pid);
    }

    // Paso 7. Detener el proceso con platform_stop_process(pid).
    //         Si falla: perror, matar, retornar -1.
    if (platform_stop_process(pid) != 0) {
        perror("[CamilOS] no pude congelar el proceso, sigue firme ");
        kill(pid, SIGKILL);
        return -1;
    }

    // Paso 8. waitpid(pid, &status, WUNTRACED) para confirmar que se detuvo.
    //         Si falla: perror, matar, retornar -1.
    int status;
    if (waitpid(pid, &status, WUNTRACED) < 0) {
        perror("[CamilOS] waitpid falló, no sé si el proceso se detuvo 😱");
        kill(pid, SIGKILL);
        return -1;
    }

    // Paso 9. Marcar el PCB como PROC_READY, incrementar process_count,
    //         llamar rq_enqueue(idx), emitir monitor_emit_created(pid, name)
    //         y si regs_valid, monitor_emit_registers(pid, pc, sp).
    //         Retornar idx.
    process_table[idx].state = PROC_READY;
    process_count++;
    rq_enqueue(idx);
    monitor_emit_created(pid, process_table[idx].name);
    if (process_table[idx].regs_valid) {
        monitor_emit_registers(pid,
            process_table[idx].registers.program_counter,
            process_table[idx].registers.stack_pointer);
    }

    printf("[CamilOS] '%s' ha nacido con PID %d, en sala de espera 👺\n",
           process_table[idx].name, pid);

    return idx;
}


// ============================================================
// [TODO 2/4] scheduler_start
// ------------------------------------------------------------
// Arranca el scheduler: desencola el primer proceso, lo pone en
// RUNNING, le manda SIGCONT e instala el timer que dispara el
// context switch periódicamente.
//
// Observable correcto: tras crear 1 proceso y llamar start, ese
// proceso empieza a producir salida en la terminal.
// ============================================================
void scheduler_start(int slice_ms) {
    // Paso 1. Si rq_is_empty(), imprimir "No hay procesos en la ready queue."
    //         y retornar.
    if (rq_is_empty()) {
        printf("[CamilOS] No hay nadie en la sala de espera, agrega procesos primero 😴 😴 😴 \n");
        return;
    }

    // Paso 2. Desencolar el primer índice con rq_dequeue().
    int idx = rq_dequeue();

    // Paso 3. Actualizar PCB del proceso entrante:
    //         - process_table[idx].state = PROC_RUNNING;
    //         - clock_gettime(CLOCK_MONOTONIC, &process_table[idx].last_started);
    //         - current_running = idx;
    process_table[idx].state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &process_table[idx].last_started);
    current_running = idx;

    // Paso 4. Reanudar el proceso con platform_resume_process(process_table[idx].pid).
    platform_resume_process(process_table[idx].pid);

    // Paso 5. Activar el scheduler y arrancar el timer:
    //         - scheduler_active = 1;
    //         - timer_init(slice_ms, scheduler_tick);  // registra el handler
    //         - timer_start();                         // arranca setitimer
    scheduler_active = 1;
    timer_init(slice_ms, scheduler_tick);
    timer_start();

    printf("[CamilOS] ¡CamilOS ha llegado! '%s' (PID %d) tiene la CPU, slice: %d ms\n",
           process_table[idx].name, process_table[idx].pid, slice_ms);
}


// ============================================================
// [TODO 3/4] scheduler_tick  — handler de SIGALRM
// ------------------------------------------------------------
// Se invoca CADA vez que expira el time slice. Realiza el
// context switch: detiene al proceso actual, actualiza su PCB,
// lo manda al final de la cola, saca al siguiente y lo reanuda.
//
// ¡IMPORTANTE! Esta función corre en un signal handler.
//   - NO llames printf/fprintf/malloc.
//   - Solo kill, waitpid, clock_gettime, write son seguros.
//   - Las funciones monitor_emit_* internamente usan snprintf + write,
//     que es aceptable para este proyecto educativo.
//
// Observable correcto: el Gantt chart del dashboard muestra
// segmentos alternados entre procesos cada ~slice_ms.
// ============================================================
void scheduler_tick(int signum) {
    (void)signum;

    // Paso 1. Salida temprana: si current_running < 0 o !scheduler_active, return.
    if (current_running < 0 || !scheduler_active) return;

    // Paso 2. Obtener puntero al PCB del proceso actual:
    //         pcb_t *current = &process_table[current_running];
    pcb_t *current = &process_table[current_running];

    // Paso 3. Detener el proceso actual con platform_stop_process(current->pid).
    platform_stop_process(current->pid);

    // Paso 4. Actualizar PCB del saliente:
    //         - Obtener tiempo actual con clock_gettime(CLOCK_MONOTONIC, &now).
    //         - Calcular elapsed = timespec_diff_ms(now, current->last_started).
    //         - current->cpu_time_ms += elapsed;
    //         - current->state = PROC_READY;
    //         - current->context_switches++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = timespec_diff_ms(now, current->last_started);
    current->cpu_time_ms += elapsed;
    current->state = PROC_READY;
    current->context_switches++;

    // Paso 5. Encolar el proceso saliente con rq_enqueue(current_running).
    clock_gettime(CLOCK_MONOTONIC, &current->last_started);
    rq_enqueue(current_running);

    // Paso 6. Si la cola quedó vacía (rq_is_empty()):
    //         - current_running = -1;
    //         - timer_stop();
    //         - return;
    if (rq_is_empty()) {
        current_running = -1;
        timer_stop();
        return;
    }

    // Paso 7. Desencolar el siguiente, actualizar su PCB y reanudarlo:
    //         - int next_idx = rq_dequeue();
    //         - pcb_t *next = &process_table[next_idx];
    //         - next->state = PROC_RUNNING;
    //         - clock_gettime(CLOCK_MONOTONIC, &next->last_started);
    //         - platform_resume_process(next->pid);
    //         - monitor_emit_switch(current->pid, next->pid, timer_get_slice());
    //         - current_running = next_idx;
    int next_idx = rq_dequeue();
    pcb_t *next = &process_table[next_idx];
    next->state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &now);
    // Acumular tiempo de espera: cuánto estuvo en la ready queue
    next->wait_time_ms += timespec_diff_ms(now, next->last_started);
    clock_gettime(CLOCK_MONOTONIC, &next->last_started);
    platform_resume_process(next->pid);
    monitor_emit_switch(current->pid, next->pid, timer_get_slice());
    current_running = next_idx;

}


// ============================================================
// [TODO 4/4] scheduler_sigchld  — handler de SIGCHLD
// ------------------------------------------------------------
// Se invoca cuando un proceso hijo termina. Debe:
//   - Detectar TODOS los hijos terminados (puede haber varios).
//   - Actualizar su PCB a PROC_TERMINATED.
//   - Si el proceso que terminó era el RUNNING, despachar al siguiente.
//   - Si estaba en la ready queue, removerlo con rq_remove().
//
// Observable correcto: tras `exit` en miniOS, `ps aux | grep defunct`
// no muestra zombies.
// ============================================================
void scheduler_sigchld(int signum) {
    (void)signum;

    // Paso 1. Loop while waitpid(-1, &status, WNOHANG | WUNTRACED) > 0:
    //         - WNOHANG para no bloquear.
    //         - El loop recoge todos los hijos terminados pendientes.
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {

        // Paso 2. Dentro del loop, IGNORAR paradas: si !WIFEXITED(status) y
        //         !WIFSIGNALED(status), continue (el proceso solo se detuvo,
        //         no terminó).
        if (!WIFEXITED(status) && !WIFSIGNALED(status)) continue;

        // Paso 3. Buscar el PID en process_table (loop por los process_count).
        //         Ignorar si ya está PROC_TERMINATED.
        for (int i = 0; i < process_count; i++) {
            if (process_table[i].pid != pid) continue;
            if (process_table[i].state == PROC_TERMINATED) continue;

            // Paso 4. Marcar process_table[i].state = PROC_TERMINATED y
            //         llamar monitor_emit_terminated(pid, cpu_time_ms, context_switches).
            process_table[i].state = PROC_TERMINATED;
            monitor_emit_terminated(pid,
                process_table[i].cpu_time_ms,
                process_table[i].context_switches);

            // Paso 5. Si i == current_running (el que terminó era el que corría):
            //         a) Actualizar cpu_time_ms con el elapsed desde last_started.
            //         b) current_running = -1;
            //         c) Si !rq_is_empty(): desencolar siguiente, marcarlo RUNNING,
            //            registrar last_started y platform_resume_process.
            //            Luego current_running = next.
            //         d) Si rq_is_empty(): timer_stop(); scheduler_active = 0;
            if (i == current_running) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                process_table[i].cpu_time_ms += timespec_diff_ms(now, process_table[i].last_started);
                current_running = -1;

                if (!rq_is_empty()) {
                    int next = rq_dequeue();
                    process_table[next].state = PROC_RUNNING;
                    clock_gettime(CLOCK_MONOTONIC, &process_table[next].last_started);
                    platform_resume_process(process_table[next].pid);
                    current_running = next;
                } else {
                    timer_stop();
                    scheduler_active = 0;
                }

            // Paso 6. Si i != current_running (estaba en la cola esperando):
            //         rq_remove(i);
            } else {
                rq_remove(i);
            }

            break;
        }
    }
}
