// ============================================================================
// INCLUSÃO DE BIBLIOTECAS E CONFIGURAÇÕES PRÉ-PROCESSADOR
// ============================================================================

// Define diretivas para otimizar a compilação no Windows e evitar conflitos de nomes entre macros
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER

#include <winsock2.h>   // Biblioteca principal do Windows para sockets de rede (Winsock 2)
#include <ws2tcpip.h>   // Extensões de protocolo e funções de conversão de endereços IP
#include <windows.h>    // API nativa do sistema operativo Windows (gestão de janelas e processos)
#include "raylib.h"     // Motor gráfico leve em C para renderização da Interface de Utilizador (UI)
#include <string>       // Classe de manipulação de cadeias de caracteres (strings) em C++
#include <vector>       // Contentor dinâmico (vetor) para gerir o histórico de mensagens do chat
#include <thread>       // Suporte a Threads nativas em C++ para execução concorrente
#include <mutex>        // Mecanismo de exclusão mútua (Mutex) para proteger dados entre threads
#include <iostream>     // Operações de entrada e saída em consola (para depuração/debug)
#include <algorithm>    // Algoritmos úteis como std::max e std::min
#include <ctime>        // Funções de manipulação de data e hora do sistema
#include <clocale>      // Biblioteca de localização para configurar o suporte a acentos (PT-BR/PT-PT)

#define PORTA 8888       // Define a porta TCP padrão em que o servidor e o cliente comunicam
#define BUFFER_SIZE 512  // Define o tamanho máximo (em bytes) do buffer de envio/receção de dados

// ============================================================================
// ESTRUTURAS DE DADOS
// ============================================================================

// Estrutura para modelar individualmente cada mensagem exibida no chat visual
struct ChatMessage {
    std::string sender;   // Nome do remetente da mensagem ("Eu", "SISTEMA" ou o nick de outros colegas)
    std::string text;     // Conteúdo textual da mensagem enviada ou recebida
    std::string timeStr;  // Horário exato em que a mensagem foi registada (ex: "14:32")
    bool isMe;            // Booleano: true se a mensagem foi enviada por ti, false se veio de terceiros
};

// ============================================================================
// VARIÁVEIS GLOBAIS DE ESTADO
// ============================================================================

SOCKET cliente_socket = INVALID_SOCKET;      // Socket principal de comunicação TCP do cliente com o servidor
bool conectado = false;                       // Flag que indica se o cliente está atualmente ligado ao servidor
std::vector<ChatMessage> historico_mensagens; // Vetor dinâmico que guarda todas as mensagens exibidas no ecrã
std::mutex mtx_mensagens;                     // Mutex para evitar conflitos de acesso concorrente ao histórico
char nickname[50] = "";                       // Armazena o apelido escolhido pelo utilizador ao entrar no chat
char ip_servidor[100] = "127.0.0.1";          // Endereço IP padrão do servidor (loopback local)
char input_texto[BUFFER_SIZE] = "";           // Armazena o texto que está a ser digitado na caixa inferior
bool na_tela_de_login = true;                 // Controlo de ecrã: true exibe o login, false exibe o chat ativo
float scroll_offset = 0.0f;                   // Regista o deslocamento atual da barra de rolagem (scroll do rato)

// ============================================================================
// FUNÇÕES AUXILIARES DE SUPORTE A UTF-8 (ACENTUAÇÃO)
// ============================================================================

// Converte um codepoint Unicode obtido pelo teclado para o formato UTF-8 e insere num buffer de C-string
void AppendCodepointToBuffer(char* buffer, int max_size, int codepoint) {
    int current_len = strlen(buffer); // Obtém o comprimento atual da string no buffer
    char utf8[5] = {0};               // Array temporário para armazenar o caractere UTF-8 resultante (até 4 bytes)
    
    // Regras de codificação UTF-8 com base no tamanho do codepoint Unicode:
    if (codepoint <= 0x7F) {
        utf8[0] = (char)codepoint; // Caracteres ASCII padrão (1 byte)
    } else if (codepoint <= 0x7FF) {
        utf8[0] = (char)(0xC0 | ((codepoint >> 6) & 0x1F)); // Caracteres de 2 bytes
        utf8[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        utf8[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F)); // Caracteres de 3 bytes (maioria dos acentos latinos)
        utf8[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        utf8[0] = (char)(0xF0 | ((codepoint >> 18) & 0x07)); // Caracteres de 4 bytes (Emojis, etc.)
        utf8[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (codepoint & 0x3F));
    }
    
    // Se houver espaço disponível no buffer, concatena o novo caractere UTF-8
    if (current_len + strlen(utf8) < (size_t)max_size) {
        strcat(buffer, utf8);
    }
}

// Remove corretamente um caractere UTF-8 completo (que pode ocupar múltiplos bytes) ao usar o Backspace
void BackspaceUTF8(char* buffer) {
    int len = strlen(buffer);
    if (len == 0) return; // Se o buffer estiver vazio, não faz nada
    
    int i = len - 1;
    // Retrocede enquanto os bytes pertencerem à sequência de continuação UTF-8 (bits 10xxxxxx)
    while (i > 0 && (buffer[i] & 0xC0) == 0x80) {
        i--;
    }
    buffer[i] = '\0'; // Corta a string no início do caractere UTF-8 detetado
}

// ============================================================================
// FUNÇÕES DE UTILIDADE E REDE
// ============================================================================

// Retorna a hora atual do sistema operacional formatada estritamente como "HH:MM"
std::string GetCurrentTimeStr() {
    time_t now = time(0);
    tm *ltm = localtime(&now);
    char buf[10];
    sprintf(buf, "%02d:%02d", ltm->tm_hour, ltm->tm_min);
    return std::string(buf);
}

// Algoritmo de quebra automática de linha (Word Wrap) para ajustar mensagens extensas à largura do balão
std::vector<std::string> WrapText(Font font, const std::string& text, float fontSize, float maxWidth) {
    std::vector<std::string> lines;
    if (text.empty()) {
        lines.push_back("");
        return lines;
    }
    
    std::string currentLine = "";
    std::string currentWord = "";
    
    // Itera por cada caractere da string de texto
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c == '\n') {
            currentLine += currentWord;
            currentWord = "";
            lines.push_back(currentLine);
            currentLine = "";
        } else if (c == ' ') {
            std::string testLine = currentLine.empty() ? currentWord : currentLine + " " + currentWord;
            // Mede em pixels a largura que a linha ocuparia no ecrã com a fonte atual
            if (MeasureTextEx(font, testLine.c_str(), fontSize, 1).x > maxWidth) {
                lines.push_back(currentLine);
                currentLine = currentWord;
            } else {
                currentLine = testLine;
            }
            currentWord = "";
        } else {
            currentWord += c;
        }
    }
    
    std::string testLine = currentLine.empty() ? currentWord : currentLine + " " + currentWord;
    if (!currentWord.empty()) {
        if (MeasureTextEx(font, testLine.c_str(), fontSize, 1).x > maxWidth && !currentLine.empty()) {
            lines.push_back(currentLine);
            lines.push_back(currentWord);
        } else {
            lines.push_back(testLine);
        }
    } else if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    
    if (lines.empty()) lines.push_back(text);
    return lines;
}

// Thread secundária executada em segundo plano para escutar mensagens vindas do servidor via socket TCP
void ThreadRecebimento() {
    char buffer[BUFFER_SIZE];
    while (conectado) {
        memset(buffer, 0, BUFFER_SIZE);
        // Bloqueia a execução à espera de dados enviados pelo servidor
        int bytes = recv(cliente_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            // Se bytes <= 0, a conexão foi encerrada abruptamente pelo servidor ou caiu a rede
            conectado = false;
            std::lock_guard<std::mutex> lock(mtx_mensagens);
            historico_mensagens.push_back({"SISTEMA", "Conexão perdida com o servidor.", GetCurrentTimeStr(), false});
            break;
        }
        buffer[bytes] = '\0';
        
        std::string msg(buffer);
        // Remove quebras de linha excedentes no final da mensagem recebida
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }

        // Processa a mensagem dividindo o remetente e o conteúdo real
        ChatMessage cm;
        cm.timeStr = GetCurrentTimeStr();
        size_t colon = msg.find(": ");
        if (colon != std::string::npos && msg.rfind("[SISTEMA]", 0) != 0) {
            cm.sender = msg.substr(0, colon);
            cm.text = msg.substr(colon + 2);
            cm.isMe = (cm.sender == std::string(nickname));
        } else {
            cm.sender = "SISTEMA";
            cm.text = msg;
            cm.isMe = false;
        }

        // Insere a mensagem obtida no vetor global de forma protegida por Mutex (Thread-safe)
        std::lock_guard<std::mutex> lock(mtx_mensagens);
        historico_mensagens.push_back(cm);
        scroll_offset = 0.0f; // Reseta o scroll para exibir a mensagem mais recente no fundo
    }
}

// Função responsável por inicializar o Winsock e estabelecer a conexão TCP com o servidor
bool ConectarServidor() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false; // Inicializa a biblioteca Winsock 2.2

    cliente_socket = socket(AF_INET, SOCK_STREAM, 0); // Cria o socket TCP
    if (cliente_socket == INVALID_SOCKET) return false;

    struct sockaddr_in servidor_addr;
    servidor_addr.sin_family = AF_INET;
    servidor_addr.sin_port = htons(PORTA);
    servidor_addr.sin_addr.s_addr = inet_addr(ip_servidor); // Atribui o IP inserido pelo utilizador

    // Tenta conectar ao endereço do servidor especificado
    if (connect(cliente_socket, (struct sockaddr*)&servidor_addr, sizeof(servidor_addr)) == SOCKET_ERROR) {
        closesocket(cliente_socket);
        return false;
    }

    // Envia imediatamente o nickname escolhido para o servidor registar
    std::string nick_envio = std::string(nickname) + "\n";
    send(cliente_socket, nick_envio.c_str(), nick_envio.length(), 0);
    conectado = true;

    // Lança a thread de receção em segundo plano de forma independente (.detach())
    std::thread(ThreadRecebimento).detach();
    return true;
}

// ============================================================================
// FUNÇÃO PRINCIPAL (MAIN) - INTERFACE GRÁFICA E CICLO DE EXECUÇÃO
// ============================================================================
int main() {
    // Configura o locale do sistema para reconhecer acentuação e caracteres em português
    setlocale(LC_ALL, "Portuguese");
    
    // Permite que a janela principal do chat seja redimensionada e maximizada pelo utilizador
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    
    const int larguraInicial = 800;
    const int alturaInicial = 600;
    InitWindow(larguraInicial, alturaInicial, "Chat Moderno - Dark Mode");
    SetTargetFPS(60); // Trava a taxa de atualização da interface gráfica em 60 Frames por Segundo

    // Carrega a fonte Arial padrão do Windows configurada para carregar até 512 caracteres (incluindo acentos)
    Font fonteChat = GetFontDefault();
    if (FileExists("C:\\Windows\\Fonts\\arial.ttf")) {
        int codepoints[512];
        for (int i = 0; i < 512; i++) codepoints[i] = i;
        fonteChat = LoadFontEx("C:\\Windows\\Fonts\\arial.ttf", 28, codepoints, 512);
        SetTextureFilter(fonteChat.texture, TEXTURE_FILTER_BILINEAR); // Aplica filtro bilinear para suavizar a fonte
    }

    // Definição da paleta de cores (Estilo Dark Mode - Padrão Discord)
    Color corFundo = { 30, 31, 36, 255 };
    Color corPainel = { 47, 49, 54, 255 };
    Color corBorda = { 79, 84, 92, 255 };
    Color corTextoGeral = { 220, 221, 222, 255 };
    Color corBotao = { 88, 101, 242, 255 };
    Color corBotaoHover = { 71, 82, 196, 255 };

    int caixaTextoAtiva = 0; // Regista o foco atual: 0 = IP, 1 = Nickname, 2 = Caixa de Chat

    // Loop principal da aplicação (executa continuamente a cada frame até fechar a janela)
    while (!WindowShouldClose()) {
        int largura = GetScreenWidth();  // Obtém a largura responsiva atual da janela
        int altura = GetScreenHeight(); // Obtém a altura responsiva atual da janela

        // --------------------------------------------------------------------
        // LÓGICA DE INPUT E EVENTOS
        // --------------------------------------------------------------------
        if (na_tela_de_login) {
            // Coordenadas dinâmicas dos elementos da tela de login
            Rectangle retIP = { (float)(largura/2 - 150), (float)(altura/2 - 60), 300, 42 };
            Rectangle retNick = { (float)(largura/2 - 150), (float)(altura/2 + 30), 300, 42 };
            Rectangle retBotaoEntrar = { (float)(largura/2 - 150), (float)(altura/2 + 100), 300, 45 };

            // Deteta cliques do rato para alternar o foco entre os campos de entrada do login
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 mousePos = GetMousePosition();
                if (CheckCollisionPointRec(mousePos, retIP)) caixaTextoAtiva = 0;
                else if (CheckCollisionPointRec(mousePos, retNick)) caixaTextoAtiva = 1;
                else caixaTextoAtiva = -1;
            }

            // Captura os caracteres do teclado com suporte a acentuação UTF-8
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32) {
                    if (caixaTextoAtiva == 0 && strlen(ip_servidor) < 99) {
                        AppendCodepointToBuffer(ip_servidor, 100, key);
                    } else if (caixaTextoAtiva == 1 && strlen(nickname) < 49) {
                        AppendCodepointToBuffer(nickname, 50, key);
                    }
                }
                key = GetCharPressed();
            }

            // Processa a tecla Backspace para apagar caracteres corretamente com suporte a UTF-8
            if (IsKeyPressed(KEY_BACKSPACE)) {
                if (caixaTextoAtiva == 0) BackspaceUTF8(ip_servidor);
                if (caixaTextoAtiva == 1) BackspaceUTF8(nickname);
            }

            // Dispara a tentativa de conexão ao premir Enter ou clicar no botão "Conectar"
            bool acaoConectar = IsKeyPressed(KEY_ENTER);
            if (CheckCollisionPointRec(GetMousePosition(), retBotaoEntrar) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                acaoConectar = true;
            }

            if (acaoConectar) {
                if (strlen(nickname) > 0 && strlen(ip_servidor) > 0) {
                    if (ConectarServidor()) {
                        na_tela_de_login = false; // Transita para a interface de chat ativo se conectar com sucesso
                    }
                }
            }
        } else {
            // Lógica de digitação e envio de mensagens no chat ativo
            Rectangle retInputChat = { 20, (float)(altura - 65), (float)(largura - 140), 42 };

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (CheckCollisionPointRec(GetMousePosition(), retInputChat)) caixaTextoAtiva = 2;
                else caixaTextoAtiva = -1;
            }

            // Captura texto digitado com suporte a acentos no chat
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && caixaTextoAtiva == 2) {
                    if (strlen(input_texto) < BUFFER_SIZE - 1) {
                        AppendCodepointToBuffer(input_texto, BUFFER_SIZE, key);
                    }
                }
                key = GetCharPressed();
            }

            if (IsKeyPressed(KEY_BACKSPACE) && caixaTextoAtiva == 2) {
                BackspaceUTF8(input_texto);
            }

            // Envia a mensagem ao premir Enter ou clicar no botão "Enviar"
            bool enviar = (IsKeyPressed(KEY_ENTER) && caixaTextoAtiva == 2);
            Rectangle retBtnEnviar = { (float)(largura - 110), (float)(altura - 65), 90, 42 };
            if (CheckCollisionPointRec(GetMousePosition(), retBtnEnviar) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                enviar = true;
            }

            if (enviar && strlen(input_texto) > 0) {
                std::string msg_texto = std::string(input_texto);
                std::string msg_envio = msg_texto + "\n";
                // Envia a string formatada através do socket TCP para o servidor
                send(cliente_socket, msg_envio.c_str(), msg_envio.length(), 0);
                
                // Adiciona a tua própria mensagem ao histórico local de mensagens
                std::lock_guard<std::mutex> lock(mtx_mensagens);
                historico_mensagens.push_back({"Eu", msg_texto, GetCurrentTimeStr(), true});
                scroll_offset = 0.0f;
                
                input_texto[0] = '\0'; // Limpa a caixa de texto inferior
            }
        }

        // --------------------------------------------------------------------
        // RENDERIZAÇÃO GRÁFICA DOS ELEMENTOS VISUAIS
        // --------------------------------------------------------------------
        BeginDrawing();
        ClearBackground(corFundo); // Limpa o ecrã pintando com a cor de fundo em Dark Mode

        bool cursorPiscando = ((int)(GetTime() * 2) % 2) == 0; // Controla a intermitência do cursor '|'

        if (na_tela_de_login) {
            // Desenho visual da Tela de Login
            DrawTextEx(fonteChat, "BEM-VINDO AO CHAT", { (float)(largura/2 - MeasureTextEx(fonteChat, "BEM-VINDO AO CHAT", 20, 1).x/2), (float)(altura/2 - 145) }, 20, 1, corTextoGeral);
            DrawTextEx(fonteChat, "Insira os dados para ligar ao servidor", { (float)(largura/2 - MeasureTextEx(fonteChat, "Insira os dados para ligar ao servidor", 13, 1).x/2), (float)(altura/2 - 112) }, 13, 1, Color{ 140, 145, 155, 255 });

            // Campo de IP
            DrawTextEx(fonteChat, "IP do Servidor:", { (float)(largura/2 - 150), (float)(altura/2 - 85) }, 13, 1, corTextoGeral);
            DrawRectangleRounded({ (float)(largura/2 - 150), (float)(altura/2 - 60), 300, 42 }, 0.3f, 4, corPainel);
            DrawRectangleRoundedLines({ (float)(largura/2 - 150), (float)(altura/2 - 60), 300, 42 }, 0.3f, 4, caixaTextoAtiva == 0 ? corBotao : corBorda);
            
            if (strlen(ip_servidor) == 0 && caixaTextoAtiva != 0) {
                DrawTextEx(fonteChat, "127.0.0.1", { (float)(largura/2 - 135), (float)(altura/2 - 49) }, 16, 1, Color{ 100, 105, 115, 255 });
            } else {
                DrawTextEx(fonteChat, ip_servidor, { (float)(largura/2 - 135), (float)(altura/2 - 49) }, 16, 1, WHITE);
                if (caixaTextoAtiva == 0 && cursorPiscando) {
                    Vector2 tSize = MeasureTextEx(fonteChat, ip_servidor, 16, 1);
                    DrawRectangle((int)(largura/2 - 135 + tSize.x + 2), (int)(altura/2 - 48), 2, 18, WHITE);
                }
            }

            // Campo de Nickname
            DrawTextEx(fonteChat, "O seu Nickname:", { (float)(largura/2 - 150), (float)(altura/2 + 5) }, 13, 1, corTextoGeral);
            DrawRectangleRounded({ (float)(largura/2 - 150), (float)(altura/2 + 30), 300, 42 }, 0.3f, 4, corPainel);
            DrawRectangleRoundedLines({ (float)(largura/2 - 150), (float)(altura/2 + 30), 300, 42 }, 0.3f, 4, caixaTextoAtiva == 1 ? corBotao : corBorda);
            
            if (strlen(nickname) == 0 && caixaTextoAtiva != 1) {
                DrawTextEx(fonteChat, "Ex: Julio", { (float)(largura/2 - 135), (float)(altura/2 + 41) }, 16, 1, Color{ 100, 105, 115, 255 });
            } else {
                DrawTextEx(fonteChat, nickname, { (float)(largura/2 - 135), (float)(altura/2 + 41) }, 16, 1, WHITE);
                if (caixaTextoAtiva == 1 && cursorPiscando) {
                    Vector2 tSize = MeasureTextEx(fonteChat, nickname, 16, 1);
                    DrawRectangle((int)(largura/2 - 135 + tSize.x + 2), (int)(altura/2 + 42), 2, 18, WHITE);
                }
            }

            // Botão de Conectar
            Rectangle retBotaoEntrar = { (float)(largura/2 - 150), (float)(altura/2 + 100), 300, 45 };
            bool hoverBotao = CheckCollisionPointRec(GetMousePosition(), retBotaoEntrar);
            DrawRectangleRounded(retBotaoEntrar, 0.3f, 4, hoverBotao ? corBotaoHover : corBotao);
            Vector2 tamanhoTextoBtn = MeasureTextEx(fonteChat, "CONECTAR", 15, 1);
            DrawTextEx(fonteChat, "CONECTAR", { retBotaoEntrar.x + retBotaoEntrar.width/2 - tamanhoTextoBtn.x/2, retBotaoEntrar.y + 14 }, 15, 1, WHITE);

        } else {
            // Desenho do Ecrã do Chat Principal
            DrawTextEx(fonteChat, TextFormat("Chat Ativo - Utilizador: %s", nickname), { 20, 15 }, 15, 1, corTextoGeral);
            DrawLine(20, 45, largura - 20, 45, corBorda);

            // Retângulo do painel de histórico de mensagens
            Rectangle retHistorico = { 20, 55, (float)(largura - 40), (float)(altura - 135) };
            DrawRectangleRounded(retHistorico, 0.03f, 4, corPainel);
            DrawRectangleRoundedLines(retHistorico, 0.03f, 4, corBorda);

            float maxBubbleWidth = retHistorico.width * 0.60f;
            float fontMsgSize = 16.0f;

            // Calcula a altura acumulada de todas as mensagens para gerir o scroll
            float totalContentHeight = 0.0f;
            {
                std::lock_guard<std::mutex> lock(mtx_mensagens);
                for (const auto& msg : historico_mensagens) {
                    std::vector<std::string> lines = WrapText(fonteChat, msg.text, fontMsgSize, maxBubbleWidth - 30);
                    float bubbleHeight = 16 + (lines.size() * 22) + (!msg.isMe ? 18 : 0) + 18; 
                    totalContentHeight += bubbleHeight + 14;
                }
            }

            float visibleHeight = retHistorico.height - 30;
            float maxScroll = std::max(0.0f, totalContentHeight - visibleHeight);

            // Lê o movimento da roda do rato para mover o histórico verticalmente
            float wheel = GetMouseWheelMove();
            if (wheel != 0) {
                scroll_offset -= wheel * 30.0f;
                if (scroll_offset < 0.0f) scroll_offset = 0.0f;
                if (scroll_offset > maxScroll) scroll_offset = maxScroll;
            }

            // Ativa o modo de recorte (Scissor Mode) para que as mensagens fiquem restritas ao painel
            BeginScissorMode((int)retHistorico.x, (int)retHistorico.y, (int)retHistorico.width, (int)retHistorico.height);
            {
                std::lock_guard<std::mutex> lock(mtx_mensagens);
                float currentY = retHistorico.y + 15 - scroll_offset + visibleHeight - totalContentHeight;

                for (const auto& msg : historico_mensagens) {
                    std::vector<std::string> lines = WrapText(fonteChat, msg.text, fontMsgSize, maxBubbleWidth - 30);
                    
                    float maxLineWidth = 0;
                    for (const auto& l : lines) {
                        float w = MeasureTextEx(fonteChat, l.c_str(), fontMsgSize, 1).x;
                        if (w > maxLineWidth) maxLineWidth = w;
                    }
                    
                    float timeWidth = MeasureTextEx(fonteChat, msg.timeStr.c_str(), 11, 1).x;
                    float bubbleWidth = std::max(maxLineWidth + 30, timeWidth + 45);
                    if (bubbleWidth < 90) bubbleWidth = 90;
                    
                    float bubbleHeight = 16 + (lines.size() * 22) + (!msg.isMe ? 18 : 0) + 18;
                    
                    float avatarOffset = (!msg.isMe && msg.sender != "SISTEMA") ? 42.0f : 0.0f;
                    float bubbleX = msg.isMe ? (retHistorico.x + retHistorico.width - bubbleWidth - 20) : (retHistorico.x + 20 + avatarOffset);

                    // Desenha apenas as mensagens que estão visíveis na área do painel atual
                    if (currentY + bubbleHeight >= retHistorico.y && currentY <= retHistorico.y + retHistorico.height) {
                        
                        // Desenha o avatar circular com a primeira letra do nome do remetente
                        if (!msg.isMe && msg.sender != "SISTEMA") {
                            float avatarX = retHistorico.x + 25 + 14;
                            float avatarY = currentY + 14;
                            DrawCircle(avatarX, avatarY, 14, corBotao);
                            char initial = msg.sender.empty() ? '?' : toupper((unsigned char)msg.sender[0]);
                            std::string initStr(1, initial);
                            Vector2 initSize = MeasureTextEx(fonteChat, initStr.c_str(), 12, 1);
                            DrawTextEx(fonteChat, initStr.c_str(), { avatarX - initSize.x/2, avatarY - initSize.y/2 }, 12, 1, WHITE);
                        }

                        // Desenha o balão da mensagem (Azul para "Eu", Cinzento para outros utilizadores)
                        Color bubbleColor = msg.isMe ? Color{ 88, 101, 242, 255 } : Color{ 54, 57, 63, 255 };
                        DrawRectangleRounded({ bubbleX, currentY, bubbleWidth, bubbleHeight }, 0.2f, 4, bubbleColor);

                        float textY = currentY + 10;
                        if (!msg.isMe && msg.sender != "SISTEMA") {
                            DrawTextEx(fonteChat, msg.sender.c_str(), { bubbleX + 14, textY }, 12, 1, Color{ 170, 175, 185, 255 });
                            textY += 18;
                        }

                        // Desenha cada linha de texto da mensagem em branco puro
                        for (const auto& l : lines) {
                            DrawTextEx(fonteChat, l.c_str(), { bubbleX + 14, textY }, fontMsgSize, 1, WHITE);
                            textY += 22;
                        }

                        // Desenha o horário (timestamp) no canto inferior direito do balão
                        DrawTextEx(fonteChat, msg.timeStr.c_str(), { bubbleX + bubbleWidth - timeWidth - 12, currentY + bubbleHeight - 17 }, 11, 1, Color{ 220, 225, 235, 220 });
                    }

                    currentY += bubbleHeight + 14;
                }
            }
            EndScissorMode(); // Termina o modo de recorte do histórico

            // Desenha a barra de rolagem lateral minimalista se necessário
            if (maxScroll > 0.0f) {
                float scrollbarTrackHeight = retHistorico.height - 10;
                float scrollbarThumbHeight = (visibleHeight / totalContentHeight) * scrollbarTrackHeight;
                if (scrollbarThumbHeight < 25.0f) scrollbarThumbHeight = 25.0f;
                float scrollbarThumbY = retHistorico.y + 5 + (scroll_offset / maxScroll) * (scrollbarTrackHeight - scrollbarThumbHeight);
                
                DrawRectangleRounded({ retHistorico.x + retHistorico.width - 8, scrollbarThumbY, 4, scrollbarThumbHeight }, 0.5f, 4, Color{ 120, 125, 135, 180 });
            }

            // Caixa de texto inferior para digitação de novas mensagens
            Rectangle retInputChat = { 20, (float)(altura - 65), (float)(largura - 130), 42 };
            DrawRectangleRounded(retInputChat, 0.3f, 4, corPainel);
            DrawRectangleRoundedLines(retInputChat, 0.3f, 4, caixaTextoAtiva == 2 ? corBotao : corBorda);
            
            if (strlen(input_texto) == 0 && caixaTextoAtiva != 2) {
                DrawTextEx(fonteChat, "Escreve uma mensagem...", { retInputChat.x + 15, retInputChat.y + 11 }, 16, 1, Color{ 100, 105, 115, 255 });
            } else {
                DrawTextEx(fonteChat, input_texto, { retInputChat.x + 15, retInputChat.y + 11 }, 16, 1, WHITE);
                if (caixaTextoAtiva == 2 && cursorPiscando) {
                    Vector2 tSize = MeasureTextEx(fonteChat, input_texto, 16, 1);
                    DrawRectangle((int)(retInputChat.x + 15 + tSize.x + 2), (int)(retInputChat.y + 12), 2, 18, WHITE);
                }
            }

            // Botão gráfico de envio de mensagem
            Rectangle retBtnEnviar = { (float)(largura - 100), (float)(altura - 65), 80, 42 };
            bool hoverEnviar = CheckCollisionPointRec(GetMousePosition(), retBtnEnviar);
            DrawRectangleRounded(retBtnEnviar, 0.3f, 4, hoverEnviar ? corBotaoHover : corBotao);
            Vector2 tamanhoEnviar = MeasureTextEx(fonteChat, "ENVIAR", 14, 1);
            DrawTextEx(fonteChat, "ENVIAR", { retBtnEnviar.x + retBtnEnviar.width/2 - tamanhoEnviar.x/2, retBtnEnviar.y + 14 }, 14, 1, WHITE);
        }

        EndDrawing(); // Encerra a renderização do frame atual
    }

    // --- ENCERRAMENTO E LIBERTAÇÃO DE RECURSOS ---
    if (conectado) {
        closesocket(cliente_socket); // Fecha o socket TCP ativo
        WSACleanup();                // Encerra a instância da Winsock
    }
    CloseWindow(); // Fecha a janela gráfica da Raylib
    return 0;      // Termina o programa com sucesso
}