#include <stdio.h>      // Biblioteca padrão de entrada e saída (printf, sprintf, etc.)
#include <stdlib.h>     // Biblioteca para gestão de memória dinâmica (malloc, free, etc.)
#include <string.h>     // Biblioteca para manipulação de cadeias de caracteres (strcpy, strlen, strncmp, etc.)
#include <winsock2.h>   // API de sockets do Windows (Winsock 2) para comunicação de rede TCP/IP
#include <ws2tcpip.h>   // Extensões de protocolo e endereçamento IP para Winsock
#include <windows.h>    // API nativa do Windows para gestão de Threads e Seções Críticas (Critical Section)

// Define a porta padrão TCP em que o servidor ficará a escutar as ligações
#define PORTA 8888
// Define a capacidade máxima de utilizadores ligados em simultâneo
#define MAX_CLIENTS 10
// Define a dimensão do buffer de memória para o envio/recebimento de dados
#define BUFFER_SIZE 1024

// Estrutura de dados que armazena a identidade e o socket de cada cliente ativo
typedef struct {
    SOCKET socket;          // Socket exclusivo de comunicação com este cliente específico
    char nickname[50];      // Nome de utilizador (apelido) escolhido pelo cliente
    int id;                 // Índice identificador numérico interno na lista de clientes
} ClienteInfo;

// Array global estático para registar a lista de todos os clientes ligados
ClienteInfo clientes[MAX_CLIENTS];
// Contador dinâmico do total de clientes atualmente ativos no chat
int total_clientes = 0;

// Seção Crítica do Windows (Mutex nativo) para evitar condições de corrida (Race Conditions) entre Threads
CRITICAL_SECTION lock_clientes;

// Função que retransmite uma mensagem para todos os clientes ativos, exceto para quem a enviou (Broadcast)
void transmitir_para_todos(char *mensagem, SOCKET remetente_socket) {
    // Entra na seção crítica para bloquear o acesso simultâneo de outras threads à lista de clientes
    EnterCriticalSection(&lock_clientes); 
    
    // Percorre todos os slots da lista global de clientes
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // Verifica se o slot possui um socket válido e se não pertence ao próprio remetente original
        if (clientes[i].socket != INVALID_SOCKET && clientes[i].socket != remetente_socket) {
            // Envia a string de mensagem via socket TCP para o cliente respetivo
            send(clientes[i].socket, mensagem, strlen(mensagem), 0);
        }
    }
    
    // Liberta a seção crítica para que outras threads possam aceder à lista global
    LeaveCriticalSection(&lock_clientes); 
}

// Função de atendimento individual para cada cliente, executada de forma concorrente numa Thread dedicada
DWORD WINAPI atendimento_cliente(LPVOID arg) {
    // Converte o ponteiro genérico recebido de volta para o tipo SOCKET
    SOCKET sock_cliente = *(SOCKET*)arg;
    // Liberta a memória dinâmica que foi alocada na thread principal (main) para passar o socket
    free(arg); 
    
    char buffer[BUFFER_SIZE]; // Buffer para armazenar as mensagens recebidas do cliente
    char nickname[50];        // Armazena o nickname temporário do cliente nesta thread
    int meu_indice = -1;      // Guarda a posição do cliente dentro do array global

    // Recebe o primeiro pacote de dados enviado pelo cliente, que contém o seu nickname
    int bytes_recebidos = recv(sock_cliente, nickname, sizeof(nickname), 0);
    if (bytes_recebidos <= 0) {
        // Se a receção falhar ou o cliente se desligar imediatamente, fecha o socket e encerra a thread
        closesocket(sock_cliente);
        return 0;
    }
    
    // Garante a terminação nula do texto recebido
    nickname[bytes_recebidos] = '\0';
    // Remove eventuais carateres de quebra de linha ('\r' ou '\n') do nickname
    nickname[strcspn(nickname, "\r\n")] = 0; 

    // Regista o cliente na lista global de forma thread-safe
    EnterCriticalSection(&lock_clientes);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // Procura por um slot livre (marcado como INVALID_SOCKET)
        if (clientes[i].socket == INVALID_SOCKET) {
            clientes[i].socket = sock_cliente;      // Atribui o socket ao slot
            strcpy(clientes[i].nickname, nickname);  // Copia o nickname registado
            clientes[i].id = i;                      // Define o ID interno
            meu_indice = i;                          // Guarda a posição local do índice
            total_clientes++;                        // Incrementa o contador de clientes ativos
            break;
        }
    }
    LeaveCriticalSection(&lock_clientes);

    // Caso não haja slots disponíveis (servidor atingiu MAX_CLIENTS)
    if (meu_indice == -1) {
        char *msg_cheio = "Servidor lotado! Conexao recusada.\n";
        send(sock_cliente, msg_cheio, strlen(msg_cheio), 0); // Avisa o cliente
        closesocket(sock_cliente);                            // Encerra a ligação
        return 0;                                             // Finaliza a thread
    }

    // Exibe a confirmação da ligação na consola do servidor
    printf("[SERVIDOR] Cliente conectado: %s (Socket %d)\n", nickname, (int)sock_cliente);
    
    // Prepara e envia uma mensagem do sistema a informar os restantes utilizadores sobre a entrada do novo elemento
    char msg_entrada[BUFFER_SIZE];
    sprintf(msg_entrada, "--> %s entrou no chat.\n", nickname);
    transmitir_para_todos(msg_entrada, sock_cliente);

    // Loop contínuo para escutar e processar as mensagens recebidas deste cliente em tempo real
    while (1) {
        // Limpa o buffer de memória preenchendo-o com zeros
        memset(buffer, 0, BUFFER_SIZE);
        
        // Bloqueia e aguarda novas mensagens do cliente
        bytes_recebidos = recv(sock_cliente, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_recebidos <= 0) {
            break; // Se bytes <= 0, o cliente desligou-se ou ocorreu um erro de ligação
        }
        
        // Garante a terminação da string recebida
        buffer[bytes_recebidos] = '\0';

        // Verifica se o cliente enviou o comando de saída explícito
        if (strncmp(buffer, "/sair", 5) == 0) {
            break; // Sai do loop para iniciar o processo de desligamento
        }

        // Formata a mensagem com o prefixo do apelido do utilizador
        char mensagem_completa[BUFFER_SIZE + 64];
        sprintf(mensagem_completa, "%s: %s", nickname, buffer);
        
        // Exibe o registo da mensagem na consola do servidor
        printf("%s", mensagem_completa);

        // Retransmite a mensagem recebida para todos os outros utilizadores ligados
        transmitir_para_todos(mensagem_completa, sock_cliente);
    }

    // --- Limpeza e Desconexão Segura do Cliente ---
    EnterCriticalSection(&lock_clientes);
    closesocket(clientes[meu_indice].socket);     // Encerra o socket do cliente
    clientes[meu_indice].socket = INVALID_SOCKET; // Marca o slot como disponível novamente
    total_clientes--;                             // Decrementa o total de utilizadores ativos
    LeaveCriticalSection(&lock_clientes);

    // Notifica a consola do servidor e os restantes utilizadores da saída deste cliente
    printf("[SERVIDOR] Cliente desconectado: %s\n", nickname);
    char msg_saida[BUFFER_SIZE];
    sprintf(msg_saida, "<-- %s saiu do chat.\n", nickname);
    transmitir_para_todos(msg_saida, INVALID_SOCKET);

    return 0; // Finaliza a execução da thread
}

// Função Principal (Ponto de Entrada da Aplicação Servidora)
int main() {
    WSADATA wsa;                           // Estrutura para dados de inicialização da Winsock
    SOCKET servidor_socket, cliente_socket; // Sockets de escuta do servidor e temporário de clientes
    struct sockaddr_in servidor_addr, cliente_addr; // Estruturas de endereço IP e porta
    int addr_len = sizeof(cliente_addr);    // Tamanho da estrutura de endereço

    // Passo 1: Inicialização da API Winsock do Windows (Versão 2.2)
    printf("[SERVIDOR] Inicializando Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[ERRO] Falha ao inicializar Winsock. Codigo: %d\n", WSAGetLastError());
        return 1;
    }

    // Passo 2: Inicializa a Seção Crítica de sincronização de Threads
    InitializeCriticalSection(&lock_clientes);

    // Passo 3: Prepara a tabela de clientes marcando todos os sockets como livres (INVALID_SOCKET)
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clientes[i].socket = INVALID_SOCKET;
    }

    // Passo 4: Criação do Socket Principal TCP (AF_INET = IPv4, SOCK_STREAM = TCP)
    servidor_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor_socket == INVALID_SOCKET) {
        printf("[ERRO] Falha ao criar socket. Codigo: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Passo 5: Configura os parâmetros do endereço do servidor
    servidor_addr.sin_family = AF_INET;         // Família de endereços IPv4
    servidor_addr.sin_addr.s_addr = INADDR_ANY; // Aceita ligações de qualquer interface de rede
    servidor_addr.sin_port = htons(PORTA);      // Converte o número da porta para a ordem de bytes de rede (Big-Endian)

    // Passo 6: Associa o Socket à porta de escuta especificada (Bind)
    if (bind(servidor_socket, (struct sockaddr*)&servidor_addr, sizeof(servidor_addr)) == SOCKET_ERROR) {
        printf("[ERRO] Falha no Bind. Codigo: %d\n", WSAGetLastError());
        closesocket(servidor_socket);
        WSACleanup();
        return 1;
    }

    // Passo 7: Coloca o Socket em modo de escuta para aceitar ligações recebidas (Listen)
    if (listen(servidor_socket, SOMAXCONN) == SOCKET_ERROR) {
        printf("[ERRO] Falha no Listen. Codigo: %d\n", WSAGetLastError());
        closesocket(servidor_socket);
        WSACleanup();
        return 1;
    }

    printf("[SERVIDOR] Servidor rodando na porta %d. Aguardando conexoes...\n", PORTA);

    // Passo 8: Loop infinito principal para aceitar múltiplos clientes continuamente
    while (1) {
        // Bloqueia a execução até que um novo cliente solicite uma ligação TCP
        cliente_socket = accept(servidor_socket, (struct sockaddr*)&cliente_addr, &addr_len);
        if (cliente_socket == INVALID_SOCKET) {
            printf("[ERRO] Falha ao aceitar conexao de cliente. Codigo: %d\n", WSAGetLastError());
            continue; // Se falhar a aceitação, continua à espera de novos pedidos
        }

        // Aloca dinamicamente um ponteiro de socket exclusivo para passar com segurança à nova thread
        SOCKET *novo_sock = malloc(sizeof(SOCKET));
        *novo_sock = cliente_socket;

        // Cria uma nova Thread nativa do Windows para tratar do cliente de forma paralela
        HANDLE thread_handle = CreateThread(NULL, 0, atendimento_cliente, novo_sock, 0, NULL);
        if (thread_handle == NULL) {
            printf("[ERRO] Falha ao criar thread para o cliente.\n");
            free(novo_sock);
            closesocket(cliente_socket);
        } else {
            // Liberta o manipulador (Handle) da thread pois esta executará autonomamente
            CloseHandle(thread_handle); 
        }
    }

    // Passo 9: Encerramento de recursos e destruição (executado se o servidor for desligado)
    closesocket(servidor_socket);
    DeleteCriticalSection(&lock_clientes);
    WSACleanup();
    return 0;
}