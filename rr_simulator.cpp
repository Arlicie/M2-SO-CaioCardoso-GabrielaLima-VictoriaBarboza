#include <bits/stdc++.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * Round Robin Scheduler Simulator (C++)
 * - Supports N CPU cores (threads)
 * - Fixed or dynamic quantum
 * - I/O block and unblock (single I/O event between two CPU bursts)
 * - Dynamic process insertion (optional stdin listener)
 * - Text Gantt output + metrics (waiting, turnaround, context switches, CPU util.)
 *
 * Build:  g++ -std=gnu++17 -O2 -pthread rr_simulator.cpp -o rr
 * Run:    ./rr --file input.txt --cores 2 --quantum 2
 *         ./rr --file input.txt --cores 4 --quantum 3 --dynamic --minq 1 --maxq 8
 *         ./rr --file input.txt --cores 2 --quantum 2 --interactive
 *
 * Input file format (pipe-separated):
 *   ID | TempoChegada | Execucao1 | Bloqueio?(S/N) | Espera | Execucao2
 * Example:
 *   P1 | 0 | 4 | S | 3 | 2
 *   P2 | 1 | 5 | N | 0 | 0
 *   P3 | 2 | 3 | S | 4 | 1
 */

using namespace std;

struct Process {
    string id;
    int arrival = 0;           // TempoChegada
    int exec1 = 0;             // primeira CPU burst
    bool hasIO = false;        // Bloqueio S/N entre exec1 e exec2
    int ioWait = 0;            // tempo de espera de I/O
    int exec2 = 0;             // segunda CPU burst

    // Dinâmica
    int phase = 1;             // 1 ou 2 (qual burst)
    int remaining = 0;         // restante do burst atual
    int quantumRemaining = 0;  // restante do quantum atual
    string state = "NOVO";     // NOVO, PRONTO, EXECUTANDO, BLOQUEADO, FINALIZADO

    // Métricas
    int startTime = -1;
    int finishTime = -1;
    long long waitTime = 0;    // tempo em fila PRONTO
    long long cpuTime = 0;     // tempo efetivamente executando
    int ctxSwitches = 0;       // número de ativações (aprox. trocas de contexto)
};

struct Options {
    string file;
    int cores = 2;
    int quantum = 2;
    bool dynamicQ = false;
    int minQ = 1;
    int maxQ = 20;
    bool interactive = false;  // habilita thread de inserção manual via stdin
};

// ---------------------------- Globals ----------------------------
static Options OPT;

static mutex mtx;                                 // protege estruturas compartilhadas
static condition_variable cv_tick, cv_all_ack;    // barreira por tick
static atomic<bool> shutdownFlag{false};          // sinal de encerramento

static int T = 0;                                 // tempo global (discreto)

// Estruturas do escalonador
static unordered_map<string, Process> PROC;       // todas as entidades
static deque<string> readyQ;                      // fila RR de prontos
static unordered_map<string, int> blocked;        // pid -> tempo restante de I/O

// Núcleos
struct Core {
    string currentPid;            // processo atualmente no core ("" se ocioso)
    vector<string> timeline;      // Gantt textual por tick (pid ou "-")
    bool acked = false;           // sinalização de barreira
};
static vector<Core> CORES;                         // tamanho = OPT.cores

static int acks = 0;                               // contagem de ACKs por tick

// Estatísticas globais
static long long busySlots = 0; // soma de ticks ocupados em todos os cores

// ---------------------------- Utilitários ----------------------------
static inline string trim(const string& s) {
    size_t i = s.find_first_not_of(" \t\r\n");
    if (i == string::npos) return "";
    size_t j = s.find_last_not_of(" \t\r\n");
    return s.substr(i, j - i + 1);
}

static vector<string> splitPipe(const string& line) {
    vector<string> out; string cur;
    for (char c : line) {
        if (c == '|') { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

static bool isFinishedAll() {
    for (auto& kv : PROC) if (kv.second.state != "FINALIZADO") return false;
    return !PROC.empty();
}

static int currentQuantum(int readySize) {
    if (!OPT.dynamicQ) return OPT.quantum;
    // Política simples: aumenta com a carga para reduzir overhead de trocas
    // q = clamp(minQ, maxQ, base + floor(readySize / cores))
    int q = OPT.quantum + max(0, readySize - (int)CORES.size());
    q = max(OPT.minQ, min(OPT.maxQ, q));
    return q;
}

// ---------------------------- Leitura de entrada ----------------------------
static void loadFile(const string& path) {
    if (path.empty()) return;
    ifstream in(path);
    if (!in) { cerr << "Erro ao abrir arquivo: " << path << "\n"; exit(1); }
    string line; int ln = 0;
    while (getline(in, line)) {
        ln++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto parts = splitPipe(line);
        if (parts.size() != 6) {
            cerr << "Linha " << ln << ": formato inválido (esperado 6 colunas)." << "\n";
            exit(1);
        }
        Process p;
        p.id = parts[0];
        p.arrival = stoi(parts[1]);
        p.exec1 = stoi(parts[2]);
        p.hasIO = (parts[3] == "S" || parts[3] == "s");
        p.ioWait = stoi(parts[4]);
        p.exec2 = stoi(parts[5]);
        p.remaining = p.exec1; // começa na fase 1
        p.state = "NOVO";
        if (PROC.count(p.id)) {
            cerr << "ID duplicado: " << p.id << "\n"; exit(1);
        }
        PROC[p.id] = std::move(p);
    }
}

// Inserção dinâmica via stdin (opcional)
static void interactiveInserter() {
    // Formato: exatamente como o arquivo, mesma linha com pipes
    // Ex.:  P4 | 7 | 3 | S | 2 | 4
    string line;
    while (!shutdownFlag.load()) {
        if (!getline(cin, line)) break; // EOF
        line = trim(line);
        if (line.empty()) continue;
        auto parts = splitPipe(line);
        if (parts.size() != 6) {
            cerr << "[interactive] Linha ignorada (formato inválido): " << line << "\n";
            continue;
        }
        Process p;
        try {
            p.id = parts[0];
            p.arrival = stoi(parts[1]);
            p.exec1 = stoi(parts[2]);
            p.hasIO = (parts[3] == "S" || parts[3] == "s");
            p.ioWait = stoi(parts[4]);
            p.exec2 = stoi(parts[5]);
            p.remaining = p.exec1;
            p.state = "NOVO";
        } catch (...) {
            cerr << "[interactive] Erro ao parsear linha: " << line << "\n";
            continue;
        }
        lock_guard<mutex> lk(mtx);
        if (PROC.count(p.id)) {
            cerr << "[interactive] ID duplicado: " << p.id << "\n";
        } else {
            PROC[p.id] = std::move(p);
            cerr << "[interactive] Processo " << parts[0] << " cadastrado para chegada em t=" << parts[1] << "\n";
        }
    }
}

// ---------------------------- Núcleo (thread) ----------------------------
static void coreWorker(int cid) {
    while (true) {
        unique_lock<mutex> lk(mtx);
        cv_tick.wait(lk, [&]{ return CORES[cid].acked == false || shutdownFlag.load(); });
        if (shutdownFlag.load() && CORES[cid].acked == false && CORES[cid].currentPid.empty()) {
            // Libera e sai
            CORES[cid].timeline.push_back("-");
            CORES[cid].acked = true;
            if (++acks == (int)CORES.size()) cv_all_ack.notify_one();
            break;
        }

        string pid = CORES[cid].currentPid;
        if (!pid.empty()) {
            auto &p = PROC[pid];
            // Executa 1 tick
            p.remaining--; p.quantumRemaining--; p.cpuTime++;
            CORES[cid].timeline.push_back(pid);
            busySlots++;

            bool released = false;
            // Verifica fim de burst / I/O / quantum
            if (p.remaining == 0) {
                if (p.phase == 1 && p.hasIO) {
                    // Vai para bloqueado (I/O)
                    p.state = "BLOQUEADO";
                    blocked[pid] = p.ioWait; // inicia espera de I/O
                    p.phase = 2;
                    p.remaining = max(0, p.exec2);
                    CORES[cid].currentPid.clear();
                    released = true;
                } else {
                    // Finaliza
                    p.state = "FINALIZADO";
                    p.finishTime = T + 1; // termina ao final do tick atual
                    CORES[cid].currentPid.clear();
                    released = true;
                }
            } else if (p.quantumRemaining == 0) {
                // Preempção RR
                p.state = "PRONTO";
                readyQ.push_back(pid);
                p.ctxSwitches++;
                CORES[cid].currentPid.clear();
                released = true;
            }

            if (!released) {
                // Continua no core no próximo tick
            }
        } else {
            // ocioso
            CORES[cid].timeline.push_back("-");
        }

        // sinaliza ACK do tick
        CORES[cid].acked = true;
        if (++acks == (int)CORES.size()) cv_all_ack.notify_one();
    }
}

// ---------------------------- Agendador (thread principal) ----------------------------
static void assignIdleCoresLocked() {
    // Atribui processos PRONTO aos núcleos ociosos (Round Robin)
    for (size_t c = 0; c < CORES.size(); ++c) {
        if (!CORES[c].currentPid.empty()) continue;
        if (readyQ.empty()) continue;
        string pid = readyQ.front(); readyQ.pop_front();
        auto &p = PROC[pid];
        p.state = "EXECUTANDO";
        if (p.startTime < 0) p.startTime = T; // primeira vez que roda
        p.quantumRemaining = currentQuantum((int)readyQ.size());
        p.ctxSwitches++; // ativação conta como troca
        CORES[c].currentPid = pid;
    }
}

static void moveArrivalsLocked() {
    for (auto &kv : PROC) {
        auto &p = kv.second;
        if (p.state == "NOVO" && p.arrival <= T) {
            p.state = "PRONTO";
            readyQ.push_back(p.id);
        }
    }
}

static void progressBlockedLocked() {
    vector<string> toReady;
    for (auto it = blocked.begin(); it != blocked.end(); ) {
        it->second--;
        if (it->second <= 0) { toReady.push_back(it->first); it = blocked.erase(it); }
        else ++it;
    }
    for (auto &pid : toReady) {
        auto &p = PROC[pid];
        p.state = "PRONTO";
        readyQ.push_back(pid);
    }
}

static void accrueWaitingLocked() {
    for (auto &kv : PROC) if (kv.second.state == "PRONTO") kv.second.waitTime++;
}

static bool everyoneFinalOrNewLocked() {
    // usado para decidir quando encerrar sem travar em espera
    for (auto &kv : PROC) {
        const auto &s = kv.second.state;
        if (s != "FINALIZADO" && s != "NOVO") return false;
    }
    return true;
}

static void parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        auto need = [&](int i){ if (i+1 >= argc) { cerr << a << " requer valor" << "\n"; exit(1);} };
        if (a == "--file") { need(i); OPT.file = argv[++i]; }
        else if (a == "--cores") { need(i); OPT.cores = max(1, stoi(argv[++i])); }
        else if (a == "--quantum") { need(i); OPT.quantum = max(1, stoi(argv[++i])); }
        else if (a == "--dynamic") { OPT.dynamicQ = true; }
        else if (a == "--minq") { need(i); OPT.minQ = max(1, stoi(argv[++i])); }
        else if (a == "--maxq") { need(i); OPT.maxQ = max(1, stoi(argv[++i])); }
        else if (a == "--interactive") { OPT.interactive = true; }
        else if (a == "--help") {
            cout << "Uso: ./rr --file input.txt --cores N --quantum Q [--dynamic --minq A --maxq B] [--interactive]\n";
            exit(0);
        }
    }
    if (OPT.cores < 2) {
        cerr << "Aviso: requisito mínimo 2 núcleos. Ajustando para 2.\n";
        OPT.cores = 2;
    }
}

static void printResults() {
    // Tempo total = tamanho do gantt (todos têm mesmo comprimento)
    int totalTime = CORES.empty() ? 0 : (int)CORES[0].timeline.size();

    cout << "\n===== Gantt por Núcleo =====\n";
    for (size_t c = 0; c < CORES.size(); ++c) {
        cout << "Núcleo " << (c+1) << ": | ";
        for (auto &s : CORES[c].timeline) cout << (s == "-" ? "-" : s) << " | ";
        cout << "\n";
    }
    cout << "Tempo :   ";
    for (int t = 0; t < totalTime; ++t) cout << t << ' ';
    cout << "\n";

    cout << "\n===== Métricas =====\n";
    cout << left << setw(8) << "Proc" << setw(10) << "Chegada" << setw(10) << "Início"
         << setw(10) << "Fim" << setw(12) << "Espera" << setw(12) << "Turnaround"
         << setw(12) << "CPU" << setw(8) << "Ctx" << "\n";
    long long sumWait=0, sumTurn=0, sumCPU=0; int n=0;
    for (auto &kv : PROC) {
        auto &p = kv.second; if (p.finishTime < 0) continue; n++;
        int turnaround = p.finishTime - p.arrival;
        sumWait += p.waitTime; sumTurn += turnaround; sumCPU += p.cpuTime;
        cout << left << setw(8) << p.id << setw(10) << p.arrival << setw(10) << p.startTime
             << setw(10) << p.finishTime << setw(12) << p.waitTime << setw(12) << turnaround
             << setw(12) << p.cpuTime << setw(8) << p.ctxSwitches << "\n";
    }
    if (n) {
        cout << fixed << setprecision(2);
        cout << "\nMédias -> Espera: " << (double)sumWait/n
             << ", Turnaround: " << (double)sumTurn/n
             << ", CPU: " << (double)sumCPU/n << "\n";
    }

    double cpuUtil = 0.0;
    if (totalTime > 0 && !CORES.empty()) {
        cpuUtil = 100.0 * (double)busySlots / (double)(totalTime * (long long)CORES.size());
    }
    cout << "Utilização de CPU: " << fixed << setprecision(1) << cpuUtil << "%\n";
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    parseArgs(argc, argv);
    loadFile(OPT.file);

    CORES.assign(OPT.cores, Core{});

    thread inserter;
    if (OPT.interactive) inserter = thread(interactiveInserter);

    // Cria threads de núcleo
    vector<thread> workers;
    for (int c = 0; c < OPT.cores; ++c) workers.emplace_back(coreWorker, c);

    // Loop de simulação (t = 0, 1, 2, ...)
    while (true) {
        unique_lock<mutex> lk(mtx);
        // 1) Chegadas e desbloqueios
        moveArrivalsLocked();
        progressBlockedLocked();

        // 2) Atribuir processos aos núcleos ociosos
        assignIdleCoresLocked();

        // 3) Acumular espera para os PRONTO
        accrueWaitingLocked();

        // 4) Disparar tick para todos os cores
        acks = 0;
        for (auto &core : CORES) core.acked = false;
        cv_tick.notify_all();

        // 5) Espera todos os núcleos processarem 1 tick
        cv_all_ack.wait(lk, []{ return acks == (int)CORES.size(); });

        // 6) Avançar tempo global
        T++;

        // 7) Condição de parada: todos finalizados
        if (isFinishedAll()) break;

        // Como segurança: se não há nada executando, nem bloqueado, e somente NOVO além do horizonte,
        // permitimos continuar para atender chegadas futuras (processos com arrival > T).
        // Para evitar loop infinito quando não há mais nada chegando, checamos se não existe processo NOVO com arrival > T.
        bool anyFuture = false;
        for (auto &kv : PROC) if (kv.second.state == "NOVO" && kv.second.arrival > T) { anyFuture = true; break; }
        bool anyRunOrBlock = false;
        for (auto &core : CORES) if (!core.currentPid.empty()) { anyRunOrBlock = true; break; }
        if (!anyRunOrBlock && blocked.empty() && !anyFuture && readyQ.empty()) break; // nada mais a fazer
    }

    // Encerrar workers
    {
        lock_guard<mutex> lk(mtx);
        shutdownFlag.store(true);
        // libera barreira para permitir saída limpa
        for (auto &core : CORES) core.acked = false;
        cv_tick.notify_all();
    }
    for (auto &th : workers) th.join();
    if (inserter.joinable()) inserter.join();

    printResults();
    return 0;
}
