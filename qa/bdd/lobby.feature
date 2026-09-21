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
    Quando eu clico Desativar som
    Entao o audio para na hora
    Quando eu clico Ativar som
    Entao o audio volta
    Quando eu clico Desativar preview
    Entao a imagem para (DShow e WASAPI param)
    E o Iniciar continua disponivel
    Quando eu clico Ativar preview
    Entao a imagem e o audio voltam

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
      | Exibir metricas            |
      | Mini overlay (barra topo)  |
      | Metricas avancadas         |
      | Infos GPU / CPU            |
      | Tela cheia                 |
      | Sincronizar imagem         |
      | Overlay live               |
      | Overlay sem fundo          |
      | Reportar bug               |
      | Reportar performance       |

  @importante @hdmi
  Cenario: Mini overlay desliga metricas avancadas
    Dado que Metricas avancadas esta ligada
    Quando eu ligo Mini overlay
    Entao Metricas avancadas e Infos GPU / CPU ficam desabilitadas
    E o HUD da sessao fica no modo barra de topo

  @importante @hdmi
  Cenario: Overlay sem fundo vale para todos os estilos
    Dado que estou no lobby
    Entao Overlay sem fundo esta sempre clicavel (nao preso ao Mini overlay)
    Quando eu marco Overlay sem fundo em compacto, mini, metricas avancadas ou overlay live
    Entao o preview e a sessao mostram so o texto, sem placa preta
    E o check permanece disponivel ao trocar de estilo

  @auto
  Cenario: Mini overlay no lobby usa CPU GPU ao vivo
    Dado o preview do lobby com Mini overlay
    Entao nao aparece i5-11400F nem 35%/55C de mentira
    E o sampler preenche o SKU e a % da maquina
    E na sessao o mesmo HUD ja era ao vivo

  @importante @hdmi
  Cenario: Console na captura e publico
    Dado que estou no lobby
    Quando eu escolho PS5, PS4, PS3, Xbox Series X, Xbox Series S, Xbox One, Switch 2 ou Switch
    Entao o checkbox Mostrar console fica desligado
    E ao marcar aparecem as opcoes (padrao PS5)
    E compacto, mini, ADV e overlay live mostram o nome colorido so quando marcado

  @importante @hdmi
  Cenario: Captura sem faixa de lobby
    Dado que estou no lobby
    Entao nao existe a opcao Ocultar lobby na captura
    Quando eu clico Iniciar processamento
    Entao a sessao nao mostra faixa de painel no topo
    E Ctrl+M, metricas e mini overlay continuam iguais

  @critico @hdmi
  Cenario: Iniciar processamento entra na captura
    Dado que ha placa, modo e preview ok
    Quando eu clico Iniciar processamento
    Entao o app grava o CFG e reinicia com --autostart
    E a sessao abre ja com DLSS 5, preset e escala escolhidos no menu

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
