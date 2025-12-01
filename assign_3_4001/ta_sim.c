// ============================================================
//  SYSC 4001 - Assignment 3
//  TA Simulation with Shared Memory + Semaphores
// ============================================================

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#define MAX_EXAMS 200
#define MAX_PATH 512
#define QUESTIONS 5

#define SHM_NAME "/ta_sim_shm_v2"
#define SEM_RUBRIC "/rubric_sem_v2"
#define SEM_LOAD "/load_sem_v2"
#define SEM_Q_BASE "/q_sem_v2_"

typedef struct {
    int student_number;
    int marked[QUESTIONS];
    char filename[MAX_PATH];
} exam_t;

typedef struct {
    char rubric[QUESTIONS];
    exam_t exams[MAX_EXAMS];
    int exam_count;
    int current_index;
    int stop; // when set, children should exit
} shared_t;

static volatile sig_atomic_t got_sigint = 0;

void sigint_handler(int sig) {
    (void)sig;
    got_sigint = 1;
}

static void msleep(double seconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}


// random double in [a,b)
double randd(double a, double b) {
    return a + (b - a) * ((double)rand() / (double)RAND_MAX);
}

void make_qsem(char *out, int q) {
    snprintf(out, MAX_PATH, "%s%d", SEM_Q_BASE, q);
}

// returns 0 on success, negative on failure
int load_rubric(const char *file, char r[QUESTIONS]) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;

    char line[256];
    int i = 0;

    while (i < QUESTIONS && fgets(line, sizeof(line), f)) {
        // expecting lines like: "1, A" or "1,A"
        char *comma = strchr(line, ',');
        if (!comma) {
            fclose(f);
            return -2;
        }

        // advance pointer after comma and skip whitespace
        char *p = comma + 1;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0' || *p == '\n') {
            fclose(f);
            return -3;
        }

        r[i++] = *p;
    }

    fclose(f);
    return (i == QUESTIONS ? 0 : -4);
}

// returns 0 on success, negative on failure
int load_exams(const char *dir, shared_t *shm) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    char *names[MAX_EXAMS];
    int n = 0;
    struct dirent *de;

    while ((de = readdir(d)) && n < MAX_EXAMS) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        names[n++] = strdup(de->d_name);
    }
    closedir(d);

    if (n == 0) {
        // no files found
        for (int i = 0; i < MAX_EXAMS; i++) shm->exams[i].student_number = -1;
        return -2;
    }

    // sort names (lexicographically)
    qsort(names, n, sizeof(char *),
          (int (*)(const void *, const void *)) strcmp);

    for (int i = 0; i < n; i++) {
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);

        exam_t *ex = &shm->exams[i];
        strncpy(ex->filename, full, MAX_PATH - 1);
        ex->filename[MAX_PATH - 1] = '\0';
        for (int q = 0; q < QUESTIONS; q++) ex->marked[q] = 0;
        ex->student_number = -1;

        FILE *f = fopen(full, "r");
        if (f) {
            char buf[128];
            if (fgets(buf, sizeof(buf), f)) {
                int num;
                if (sscanf(buf, "%d", &num) == 1)
                    ex->student_number = num;
            }
            fclose(f);
        }
        free(names[i]);
    }

    shm->exam_count = n;
    return 0;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <num_TAs> <exams_dir> <rubric_file> [--sync]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, sigint_handler);

    int numTAs = atoi(argv[1]);
    const char *exam_dir = argv[2];
    const char *rubric_file = argv[3];
    int use_sync = (argc >= 5 && strcmp(argv[4], "--sync") == 0);

    if (numTAs <= 0) {
        fprintf(stderr, "num_TAs must be positive\n");
        return 1;
    }

    // create/open shared memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(fd, sizeof(shared_t)) != 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    shared_t *shm = mmap(NULL, sizeof(shared_t),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(SHM_NAME);
        return 1;
    }
    close(fd); // fd not needed after mmap

    // zero shared memory
    memset(shm, 0, sizeof(shared_t));
    shm->current_index = 0;
    shm->stop = 0;

    // load rubric
    if (load_rubric(rubric_file, shm->rubric) != 0) {
        fprintf(stderr, "Rubric load failed. Check file: %s\n", rubric_file);
        munmap(shm, sizeof(shared_t));
        shm_unlink(SHM_NAME);
        return 1;
    }

    // load exams
    if (load_exams(exam_dir, shm) != 0) {
        fprintf(stderr, "Exam folder load failed or empty: %s\n", exam_dir);
        munmap(shm, sizeof(shared_t));
        shm_unlink(SHM_NAME);
        return 1;
    }

    printf("Loaded %d exam files.\n", shm->exam_count);

    // semaphores (optional sync)
    sem_t *sem_rubric = NULL, *sem_load = NULL, *sem_q[QUESTIONS];
    // initialize pointers to NULL
    for (int q = 0; q < QUESTIONS; q++) sem_q[q] = NULL;

    if (use_sync) {
        // try to unlink first to avoid stale semaphores (ignore errors)
        sem_unlink(SEM_RUBRIC);
        sem_unlink(SEM_LOAD);
        for (int q = 0; q < QUESTIONS; q++) {
            char name[MAX_PATH];
            make_qsem(name, q);
            sem_unlink(name);
        }

        sem_rubric = sem_open(SEM_RUBRIC, O_CREAT | O_EXCL, 0666, 1);
        if (sem_rubric == SEM_FAILED) {
            // try open without O_EXCL if already created by other instance
            sem_rubric = sem_open(SEM_RUBRIC, O_CREAT, 0666, 1);
        }
        if (sem_rubric == SEM_FAILED) { perror("sem_open rubric"); goto cleanup_shm; }

        sem_load = sem_open(SEM_LOAD, O_CREAT | O_EXCL, 0666, 1);
        if (sem_load == SEM_FAILED) sem_load = sem_open(SEM_LOAD, O_CREAT, 0666, 1);
        if (sem_load == SEM_FAILED) { perror("sem_open load"); goto cleanup_sems; }

        for (int q = 0; q < QUESTIONS; q++) {
            char name[MAX_PATH];
            make_qsem(name, q);
            sem_q[q] = sem_open(name, O_CREAT | O_EXCL, 0666, 1);
            if (sem_q[q] == SEM_FAILED) sem_q[q] = sem_open(name, O_CREAT, 0666, 1);
            if (sem_q[q] == SEM_FAILED) { perror("sem_open q"); goto cleanup_sems; }
        }

        printf("Synchronization: ON\n");
    } else {
        printf("Synchronization: OFF\n");
    }

    // fork TA processes
    pid_t *kids = calloc(numTAs, sizeof(pid_t));
    if (!kids) { perror("calloc"); goto cleanup_sems; }

    for (int t = 0; t < numTAs; t++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            // continue trying to spawn others? break
            break;
        }

        if (pid == 0) {
            // child
            // reseed RNG so children are not identical
            srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

            int id = t + 1;
            while (!shm->stop && !got_sigint) {
                int idx;

                if (use_sync) {
                    if (sem_wait(sem_load) != 0) { perror("sem_wait load (child)"); break; }
                    idx = shm->current_index;
                    if (sem_post(sem_load) != 0) { perror("sem_post load (child)"); break; }
                } else {
                    idx = shm->current_index;
                }

                if (idx >= shm->exam_count) {
                    // no more exams yet, sleep a bit then retry
                    msleep(0.100);; // 100 ms
                    continue;
                }

                exam_t *ex = &shm->exams[idx];

                if (ex->student_number == 9999) {
                    // termination marker
                    shm->stop = 1;
                    break;
                }

                // RUBRIC CHECK + POSSIBLE MODIFY
                for (int q = 0; q < QUESTIONS; q++) {
                    msleep(randd(0.5, 1.0));

                    int change = rand() % 3 == 0; // 1/3 chance
                    if (!change) continue;

                    if (use_sync) {
                        if (sem_wait(sem_rubric) != 0) { perror("sem_wait rubric (child)"); break; }
                    }

                    char old = shm->rubric[q];
                    char newc = (old == 'Z' ? 'A' : old + 1);
                    shm->rubric[q] = newc;

                    // persist rubric to file (best-effort)
                    FILE *rf = fopen(rubric_file, "w");
                    if (rf) {
                        for (int i = 0; i < QUESTIONS; i++)
                            fprintf(rf, "%d, %c\n", i + 1, shm->rubric[i]);
                        fclose(rf);
                    } else {
                        // optional: print but don't abort
                        fprintf(stderr, "[TA %d] warning: couldn't open rubric file for writing: %s\n", id, strerror(errno));
                    }

                    printf("[TA %d] changed rubric Q%d: %c -> %c\n", id, q+1, old, newc);

                    if (use_sync) {
                        if (sem_post(sem_rubric) != 0) { perror("sem_post rubric (child)"); break; }
                    }
                }

                // MARK QUESTIONS
                for (int q = 0; q < QUESTIONS; q++) {
                    int can = 0;
                    if (use_sync) {
                        if (sem_wait(sem_q[q]) != 0) { perror("sem_wait q (child)"); break; }
                        if (!ex->marked[q]) {
                            ex->marked[q] = 1;
                            can = 1;
                        }
                        if (sem_post(sem_q[q]) != 0) { perror("sem_post q (child)"); break; }
                    } else {
                        if (!ex->marked[q]) { ex->marked[q] = 1; can = 1; }
                    }

                    if (can) {
                        printf("[TA %d] marking exam %d Q%d...\n", id, ex->student_number, q+1);
                        msleep(randd(1.0,2.0));
                    }
                }

                // LOAD NEXT EXAM (only done by process that sees exam is fully marked)
                if (use_sync) {
                    if (sem_wait(sem_load) != 0) { perror("sem_wait load (child2)"); break; }
                }

                int done = 1;
                for (int q = 0; q < QUESTIONS; q++)
                    if (!ex->marked[q]) done = 0;

                if (done) {
                    shm->current_index++;
                    // check for termination marker next exam
                    if (shm->current_index < shm->exam_count &&
                        shm->exams[shm->current_index].student_number == 9999) {
                        shm->stop = 1;
                    }
                }

                if (use_sync) {
                    if (sem_post(sem_load) != 0) { perror("sem_post load (child2)"); break; }
                }

                msleep(0.200); // 200 ms small pause
            }

            // child exit
            _exit(0);
        } else {
            // parent
            kids[t] = pid;
        }
    }

    // parent waits for children
    for (int i = 0; i < numTAs; i++) {
        if (kids[i] > 0) waitpid(kids[i], NULL, 0);
    }

    free(kids);

    printf("\nAll TAs finished or terminated.\n");

    // cleanup semaphores if we created them
cleanup_sems:
    if (use_sync) {
        if (sem_rubric && sem_rubric != SEM_FAILED) { sem_close(sem_rubric); sem_unlink(SEM_RUBRIC); }
        if (sem_load && sem_load != SEM_FAILED)   { sem_close(sem_load);   sem_unlink(SEM_LOAD); }
        for (int q = 0; q < QUESTIONS; q++) {
            char name[MAX_PATH];
            make_qsem(name, q);
            if (sem_q[q] && sem_q[q] != SEM_FAILED) { sem_close(sem_q[q]); sem_unlink(name); }
        }
    }

cleanup_shm:
    // unmap and unlink shared memory
    if (shm && shm != MAP_FAILED) {
        munmap(shm, sizeof(shared_t));
        shm_unlink(SHM_NAME);
    }

    return 0;
}
