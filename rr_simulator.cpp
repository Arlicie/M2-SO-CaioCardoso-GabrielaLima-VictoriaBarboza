#include <bits/stdc++.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

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

// ---------------------------- Núcleo (thread) ----------------------------
// esssa função é executada por uma thread separada para cada núcleo da CPU.
static void coreWorker(int cid) {  // o parâmetro cid (Core ID) identifica qual núcleo está sendo simulado
    while (true) { // cada núcleo trabalha dentro de um loop infinito, simula o funcionamento contínuo do processador e só sai do loop quando o simulador for encerrado (controlado pelo shutdownFlag)
        unique_lock<mutex> lk(mtx); // é usado mutex para coordenar o acesso entre threads
        cv_tick.wait(lk, [&]{ return CORES[cid].acked == false || shutdownFlag.load(); }); // cv_tick faz com que a thread(nucleo) durma até o proximo tempo de tick comece
        // o lambda é a condição de espera, só segue quando o tick for atual for confirmado (mais pra baixo) ou quandoo o simulador estiver encerrado

        // verifica se o simulador foi encerrado (shutdownFlag ativado) e o núcleo está ocioso (currentPid.empty())
        if (shutdownFlag.load() && CORES[cid].acked == false && CORES[cid].currentPid.empty()) {
            // Libera e sai
            CORES[cid].timeline.push_back("-"); // adiciona um - na linha do tempo que indica inatividade
            CORES[cid].acked = true; // reconhece o tick atual
            if (++acks == (int)CORES.size()) cv_all_ack.notify_one(); // incrementa o contador global
            break;
        }

        string pid = CORES[cid].currentPid; // pega o id do processo atual e se estiver vazio, significa que o núcleo não tem processo atribuído nesse tick
        if (!pid.empty()) { // se o núcleo tem um processo ativo, buscamos a estrutura p correspondente no dicionário global PROC (tabela de processos)
            auto &p = PROC[pid];

            // Executa 1 tick
            p.remaining--; p.quantumRemaining--; p.cpuTime++; 
            // remaining-- diminui o tempo restante total de CPU para esse processo
            // uantumRemaining-- diminui o quantum restante (quanto tempo ainda pode executar antes da preempção)
            // p.cpuTime++ contabiliza o tempo de CPU usado

            CORES[cid].timeline.push_back(pid); // o ID do processo é registrado na linha do tempo (timeline) para o gráfico de Gantt
            busySlots++; // registra que o núcleo esteve ocupado nesse tick (para calcular uso da CPU depois)

            bool released = false; 
            // variável de controle: vai indicar se o processo saiu do núcleo (por término, bloqueio ou preempção)

            // Verifica fim de burst / I/O / quantum
            if (p.remaining == 0) {
                if (p.phase == 1 && p.hasIO) { // se ele ainda está na primeira fase (phase == 1) e possui operação de entrada/saída (I/O)
                    p.state = "BLOQUEADO"; // vai para bloqueado (I/O)
                    blocked[pid] = p.ioWait; // é colocado na lista blocked com seu tempo de espera (p.ioWait)
                    p.phase = 2; // vai para a segunda fase (phase = 2) que será executada depois que o I/O terminar
                    p.remaining = max(0, p.exec2); // o tempo de CPU da segunda fase é definido (p.remaining = p.exec2)
                    CORES[cid].currentPid.clear(); // o núcleo é liberado (currentPid.clear())
                    released = true; // marca released = true
                } else {
                    // se não há mais I/O, o processo foi completamente executado
                    p.state = "FINALIZADO"; // estado fica como finalizado
                    p.finishTime = T + 1; // termina ao final do tick atual
                    CORES[cid].currentPid.clear();
                    released = true;
                }
            } else if (p.quantumRemaining == 0) { // se o quantum acabou, aplica a preempção Round Robin
                // Preempção RR
                p.state = "PRONTO"; // estado marcado como pronto
                readyQ.push_back(pid); // processo volta para a fila de prontos
                p.ctxSwitches++; // mais uma troca de conteto
                CORES[cid].currentPid.clear(); // nucleo é liberado
                released = true;
            }

            if (!released) {
                // continua no core no próximo tick
            }
        } else {
            // caso não haja processo atribuído ao núcleo ele fica ocioso e isso é marcado no gaant
            CORES[cid].timeline.push_back("-");
        }

        // sinaliza ACK do tick e acorda a thread principal
        CORES[cid].acked = true;
        if (++acks == (int)CORES.size()) cv_all_ack.notify_one();
    }
}

// ---------------------------- Agendador (thread principal) ----------------------------
static void assignIdleCoresLocked() {
    // Atribui processos PRONTO aos núcleos ociosos (Round Robin)

    for (size_t c = 0; c < CORES.size(); ++c) { // loop sobre todos os núcleos (CORES)

        // se o núcleo já possui um processo atribuído (currentPid não vazio), pula para o próximo núcleo, só considera núcleos ociosos
        if (!CORES[c].currentPid.empty()) continue; 

        // se não há nenhum processo pronto na fila (readyQ vazia), pula; nenhum trabalho a atribuir
        if (readyQ.empty()) continue;


        // retira o primeiro processo da fila de prontos (FIFO). readyQ.front() obtém o id do processo; pop_front() remove esse id da fila. 
        // isso implementa a ordem de chegada/re-entradas do Round Robin.
        string pid = readyQ.front(); readyQ.pop_front();

        // obtém referência direta ao registro do processo na tabela PROC
        auto &p = PROC[pid];

        // marca o sate como exceuctando
        p.state = "EXECUTANDO";

        // Se essa é a primeira vez que o processo foi executado (startTime inicializado com valor negativo), 
        // grava o tempo atual T como tempo de início, oq serve para métricas como tempo de resposta/turnaround
        if (p.startTime < 0) p.startTime = T;

        // Define o quantum (fatia de tempo) que o processo receberá neste escalonamento e 
        // a função currentQuantum calcula o quantum de acordo com política (fixo ou dinâmico)
        p.quantumRemaining = currentQuantum((int)readyQ.size());

        p.ctxSwitches++; // ativação conta como troca

        // atribui o prcesso ao núcleo, grava o pid em CORES[c].currentPid, 
        // indica que o núcleo c dverá executar esse processo no próximo tick
        CORES[c].currentPid = pid;
    }
}

static void moveArrivalsLocked() {
    for (auto &kv : PROC) { // itera sobre todos os processos armazenados em PROC
        auto &p = kv.second;// cra uma referência p para o processo (valor do map), facilitando o acesso às suas variáveis sem precisar escrever kv.second altas vezes
        if (p.state == "NOVO" && p.arrival <= T) { // condição que verifica se o processo ainda não entrou no sistema, 
        // ou seja, foi declarado, mas nunca ficou pronto para execução e também verifica o tempo atual da simulação (T) atingiu ou ultrapassou o tempo de chegada programado (arrival) do processo.
            p.state = "PRONTO";
            readyQ.push_back(p.id); // insere o processo na fila de prontos no final da fila
        }
    }
}


static void progressBlockedLocked() {
    vector<string> toReady; // lista temporária para processos que vão voltar ao estado PRONTO
    for (auto it = blocked.begin(); it != blocked.end(); ) {  
        it->second--; // decrementa o tempo restante de bloqueio (I/O) do processo
        if (it->second <= 0) { // se o tempo de I/O terminou
            toReady.push_back(it->first); // guarda o PID para mover depois
            it = blocked.erase(it); // remove da lista de bloqueados e avança o iterador
        } else {
            ++it; // senão, só avança o iterador
        }
    }
    for (auto &pid : toReady) { // para cada processo que terminou I/O
        auto &p = PROC[pid]; // obtém o PCB do processo
        p.state = "PRONTO"; // muda estado para PRONTO
        readyQ.push_back(pid); // coloca o PID na fila de prontos
    }
}


static void accrueWaitingLocked() {
    for (auto &kv : PROC) // percorre todos os processos
        if (kv.second.state == "PRONTO") // se o processo está pronto, esperando CPU
            kv.second.waitTime++; // incrementa tempo de espera
}

static bool everyoneFinalOrNewLocked() {
    // usado para saber se já podemos encerrar a simulação sem deadlock
    for (auto &kv : PROC) { // percorre todos os processos
        const auto &s = kv.second.state; // lê o estado atual
        if (s != "FINALIZADO" && s != "NOVO") // se alguém ainda está executando, pronto, ou bloqueado
            return false; // não terminou ainda
    }
    return true; // todos estão FINALIZADOS ou nunca chegaram -> fim seguro
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
        // else if (a == "--interactive") { OPT.interactive = true; }
        else if (a == "--help") {
            cout << "Uso: ./rr --file input.txt --cores N --quantum Q [--dynamic --minq A --maxq B] \n";
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
    // if (OPT.interactive) inserter = thread(interactiveInserter);


    //**
    // Cria threads de núcleo
    // é uma função que calcula o quantum com base na quantidade de processos prontos (readyCount),
    // ou seja, o tamanho da fila de prontos no momento, se não houver processos prontos, o quantum padrão é 1
    // a regra é para que cada 3 processos prontos adicionais, o quantum é aumentado em 1 unidade.
    //  */
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
