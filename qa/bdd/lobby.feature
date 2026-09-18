# language: pt
Funcionalidade: Tela principal (lobby)
  Como utilizador
  Quero ver o sinal HDMI, ajustar a sessao e iniciar a captura
  Sem abrir o Ctrl+M (isso e so na captura)

  @critico @hdmi
  Cenario: Lista de placas e modos NV12
    Dado que uma placa de captura esta ligada
    Quando eu abro o NexusCap no lobby
    Entao a lista mostra a placa
    E so entram modos que anunciam NV12
    Quando eu clico Recarregar entradas
    Entao a lista atualiza sem fechar o app

  @critico @hdmi
  Cenario: Preview ao vivo e audio
    Dado que a placa tem sinal HDMI
    Quando o lobby esta aberto
    Entao o preview mostra imagem (nao tela branca)
    E o audio HDMI toca se o mute do preview estiver off
    Quando eu mudo o mute do preview
    Entao o audio para ou volta na hora

  @critico @hdmi
  Cenario: Botao CFG e F12 abrem o CFG do menu principal
    Dado que estou no lobby
    Quando eu clico CFG ou aperto F12
    Entao abre o painel Configuracoes (CFG) ao lado do preview
    E isto nao e o menu Ctrl+M da captura

  @critico @hdmi
  Cenario: Esc no lobby nao mata o app
    Dado que estou no lobby e o CFG do menu esta fechado
    Quando eu aperto Esc
    Entao o NexusCap continua no lobby
    Dado que o CFG do menu principal esta aberto
    Quando eu aperto Esc
    Entao so o painel CFG fecha

  @importante @hdmi
  Esquema do Cenario: Toggles da coluna SESSAO
    Dado que estou no lobby
    Quando eu marco ou desmarco <opcao>
    Entao o estado da UI muda na hora
    E depois de Salvar no CFG ou Iniciar o valor persiste

    Exemplos:
      | opcao                      |
      | Exibir FPS (HUD)           |
      | Mini overlay (barra topo)  |
      | Metricas avancadas         |
      | Infos GPU / CPU            |
      | Tela cheia                 |
      | Ocultar lobby na captura   |
      | Sincronizar imagem         |
      | Overlay live               |
      | Reportar bug               |
      | Reportar performance       |

  @importante @hdmi
  Cenario: Mini overlay desliga metricas avancadas
    Dado que Metricas avancadas esta ligada
    Quando eu ligo Mini overlay
    Entao Metricas avancadas e Infos GPU / CPU ficam desabilitadas
    E o HUD da sessao fica no modo barra de topo

  @importante @hdmi
  Cenario: Overlay live escolhe consola
    Dado que Overlay live esta ligado
    Quando eu escolho PS5 ou Xbox ou Switch 2
    Entao o combo mostra a consola
    E na sessao o overlay live usa essa cor e o nome certo

  @critico @hdmi
  Cenario: Iniciar processamento entra na captura
    Dado que ha placa, modo e preview ok
    Quando eu clico Iniciar processamento
    Entao a sessao de captura abre
    E se a escala do Feed mudou o app reinicia com --autostart e entra na sessao

  @importante @hdmi
  Cenario: Sair fecha o app
    Dado que estou no lobby
    Quando eu clico Sair
    Entao o processo termina

  @critico @hdmi
  Cenario: Primeira instalacao ja abre com 1x e NR ativo
    Dado que nao existe settings.ini (primeira vez)
    Quando eu abro o app e inicio a sessao com RenoDX
    Entao o Pass Count e 1x
    E o Neural Rendering esta ON
