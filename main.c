#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <utime.h>
#include <sys/mman.h>
#include <syslog.h>

#define ROZMIAR_BUFORA 4096

volatile sig_atomic_t sygnal_obudzenia = 0;

void obudz(int sygnal) {
    sygnal_obudzenia = 1;
}

void Pobierz_aktualna_date(char *bufor, size_t rozmiar) {
    time_t teraz = time(NULL);
    struct tm czas;
    localtime_r(&teraz, &czas);
    strftime(bufor, rozmiar, "%Y-%m-%d %H:%M:%S", &czas);
}

int Czy_folder(const char *sciezka) {
    struct stat st;
    return stat(sciezka, &st) == 0 && S_ISDIR(st.st_mode);
}

int Usuwanie_rekurencyjne(const char *sciezka) {
    struct stat st;
    if (stat(sciezka, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(sciezka);
        if (!dir) return -1;

        struct dirent *wejscie;
        char podsciezka[PATH_MAX];

        while ((wejscie = readdir(dir)) != NULL) {
            if (strcmp(wejscie->d_name, ".") == 0 || strcmp(wejscie->d_name, "..") == 0)
                continue;

            snprintf(podsciezka, sizeof(podsciezka), "%s/%s", sciezka, wejscie->d_name);
            Usuwanie_rekurencyjne(podsciezka);
        }
        closedir(dir);
        return rmdir(sciezka);
    } else {
        return remove(sciezka);
    }
}

void Demonizuj() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_RDWR);

    char data[64];
    Pobierz_aktualna_date(data, sizeof(data));
    openlog("syncdemon", LOG_PID, LOG_DAEMON);
    syslog(LOG_INFO, "Demon uruchomiony [%s]", data);
}

void Synchronizuj_foldery(const char *katalog_zrodlowy, const char *katalog_docelowy, int rekurencja, size_t prog) {
    DIR *zrodlo = opendir(katalog_zrodlowy);
    if (!zrodlo) {
        perror("Wystąpił błąd podczas otwierania katalogu źródłowego.");
        return;
    }

    char data[64];
    struct dirent *wejscie;
    while ((wejscie = readdir(zrodlo)) != NULL) {
        if (strcmp(wejscie->d_name, ".") == 0 || strcmp(wejscie->d_name, "..") == 0)
            continue;

        char sciezka_zrodlowa[PATH_MAX];
        char sciezka_docelowa[PATH_MAX];
        snprintf(sciezka_zrodlowa, sizeof(sciezka_zrodlowa), "%s/%s", katalog_zrodlowy, wejscie->d_name);
        snprintf(sciezka_docelowa, sizeof(sciezka_docelowa), "%s/%s", katalog_docelowy, wejscie->d_name);

        struct stat stat_zrodlo;
        if (stat(sciezka_zrodlowa, &stat_zrodlo) != 0)
            continue;

        if (S_ISDIR(stat_zrodlo.st_mode)) {
            if (rekurencja) {
                mkdir(sciezka_docelowa, 0755);
                Synchronizuj_foldery(sciezka_zrodlowa, sciezka_docelowa, rekurencja, prog);
            }
            continue;
        }

        if (!S_ISREG(stat_zrodlo.st_mode))
            continue;

        struct stat stat_docel;
        int docelowy_istnieje = stat(sciezka_docelowa, &stat_docel) == 0;
        int trzeba_skopiowac = !docelowy_istnieje || difftime(stat_zrodlo.st_mtime, stat_docel.st_mtime) > 0;

        if (trzeba_skopiowac) {
            int wejscie_fd = open(sciezka_zrodlowa, O_RDONLY);
            int wyjscie_fd = open(sciezka_docelowa, O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (wejscie_fd < 0 || wyjscie_fd < 0) {
                perror("Wystąpił błąd podczas otwierania pliku.");
                if (wejscie_fd >= 0) close(wejscie_fd);
                if (wyjscie_fd >= 0) close(wyjscie_fd);
                continue;
            }

            if (stat_zrodlo.st_size < prog) {
                char bufor[ROZMIAR_BUFORA];
                ssize_t bajty;
                while ((bajty = read(wejscie_fd, bufor, ROZMIAR_BUFORA)) > 0) {
                    write(wyjscie_fd, bufor, bajty);
                }
            } else {
                size_t rozmiar = stat_zrodlo.st_size;
                void *dane = mmap(NULL, rozmiar, PROT_READ, MAP_PRIVATE, wejscie_fd, 0);
                if (dane == MAP_FAILED) {
                    perror("Wystąpił błąd podczas mapowania pliku");
                    close(wejscie_fd);
                    continue;
                }
                write(wyjscie_fd, dane, rozmiar);
                munmap(dane, rozmiar);
            }

            close(wejscie_fd);
            close(wyjscie_fd);

            struct utimbuf czasy;
            czasy.actime = stat_zrodlo.st_atime;
            czasy.modtime = stat_zrodlo.st_mtime;
            utime(sciezka_docelowa, &czasy);

            Pobierz_aktualna_date(data, sizeof(data));
            syslog(LOG_INFO, "Skopiowano z katalogu źródłowego: %s [%s]", wejscie->d_name, data);
        }
    }

    closedir(zrodlo);

    DIR *docel = opendir(katalog_docelowy);
    if (!docel) {
        perror("Wystąpił błąd podczas otwierania katalogu docelowego.");
        return;
    }

    while ((wejscie = readdir(docel)) != NULL) {
        if (strcmp(wejscie->d_name, ".") == 0 || strcmp(wejscie->d_name, "..") == 0)
            continue;

        char sciezka_zrodlowa[PATH_MAX];
        char sciezka_docelowa[PATH_MAX];
        snprintf(sciezka_zrodlowa, sizeof(sciezka_zrodlowa), "%s/%s", katalog_zrodlowy, wejscie->d_name);
        snprintf(sciezka_docelowa, sizeof(sciezka_docelowa), "%s/%s", katalog_docelowy, wejscie->d_name);

        struct stat stat_tmp;
        if (stat(sciezka_zrodlowa, &stat_tmp) != 0) {
            if (rekurencja) {
                if (Usuwanie_rekurencyjne(sciezka_docelowa) == 0) {
                    Pobierz_aktualna_date(data, sizeof(data));
                    syslog(LOG_INFO, "Usunięto z katalogu docelowego: %s [%s]", wejscie->d_name, data);
                }
            } else {
                Pobierz_aktualna_date(data, sizeof(data));
                if (unlink(sciezka_docelowa) == 0) {
                    syslog(LOG_INFO, "Usunięto z katalogu docelowego: %s [%s]", wejscie->d_name, data);
                }
            }
        }
    }

    closedir(docel);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Użycie: %s <ścieżka_źródłowa> <ścieżka_docelowa> [czas_snu_w_sekundach] [Rekurencja '-R'] [Próg rozmiaru]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *sciezka_zrodlowa = argv[1];
    const char *sciezka_docelowa = argv[2];

    if (!Czy_folder(sciezka_zrodlowa) || !Czy_folder(sciezka_docelowa)) {
        fprintf(stderr, "Błąd: Jedna ze ścieżek nie jest katalogiem.\n");
        exit(EXIT_FAILURE);
    }

    int czas_spania = 300;
    if (argc >= 4) {
        czas_spania = atoi(argv[3]);
        if (czas_spania <= 0) {
            fprintf(stderr, "Błędny czas snu.\n");
            exit(EXIT_FAILURE);
        }
    }

    int rekurencja = 0;
    if (argc >= 5) {
        if (strcmp(argv[4], "-R") == 0) {
            rekurencja = 1;
        }
    }

    size_t prog = 104857600;
    if (argc >= 6) {
        prog = atoi(argv[5]);
        if (prog <= 0) {
            fprintf(stderr, "Błędny próg rozmiaru.\n");
            exit(EXIT_FAILURE);
        }
    }

    Demonizuj();
    signal(SIGUSR1, obudz);

    while (1) {
        if (sygnal_obudzenia) {
            char data[64];
            Pobierz_aktualna_date(data, sizeof(data));
            syslog(LOG_INFO, "Demon się wybudził. [%s]", data);

            Synchronizuj_foldery(sciezka_zrodlowa, sciezka_docelowa, rekurencja, prog);
            sygnal_obudzenia = 0;
        } else {
            char data[64];
            Pobierz_aktualna_date(data, sizeof(data));
            syslog(LOG_INFO, "Demon zasypia. [%s]", data);
            sleep(czas_spania);
            sygnal_obudzenia = 1;
        }
    }
}
