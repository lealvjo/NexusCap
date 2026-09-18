# language: pt
Funcionalidade: Distribuicao e caminhos criticos
  Como quem vai mandar o instalador
  Quero os fluxos que se o utilizador falhar a app parece partida

  @critico @hdmi
  Cenario: Instalador na pasta do utilizador
    Quando eu instalo NexusCap-Setup
    Entao o app fica em %LOCALAPPDATA%\NexusCap
    E nao em Program Files
    E o RenoDX consegue gravar ini e cfg ao lado do exe

  @critico @hdmi
  Cenario: HDMI real sem tela branca
    Dado que a consola manda HDMI num modo NV12 da lista
    Quando eu escolho esse modo e inicio
    Entao a captura mostra o jogo
    E o overlay nao fica em tela branca
    E o FPS fica perto do modo escolhido
    E o estado STABLE bate com o alvo (hitch escala com o fps)

  @importante @hdmi
  Cenario: Reportar bug gera ficheiro
    Dado que Reportar bug esta ligado na coluna SESSAO
    Quando eu inicio e uso a sessao um pouco
    Entao aparece um .txt em report/ ao lado do exe

  @importante @hdmi
  Cenario: Reportar performance gera ficheiro
    Dado que Reportar performance esta ligado
    Quando eu inicio e uso a sessao um pouco
    Entao aparece um .txt em performance/ ao lado do exe

  @importante @hdmi
  Cenario: Frame Gen ausente nao derruba o app
    Dado que o Lossless Scaling nao esta instalado
    Quando eu tento ligar Frame Gen no lobby ou na sessao
    Entao aparece o aviso de exe nao encontrado
    E o NexusCap continua a correr

  @auto
  Cenario: Suite automatica de release
    Quando eu corro ctest -C Release no build
    Entao passam unit, smoke, smoke_fg_off, smoke_no_lobby, selftest_fg e selftest_cfg
    Mas os cenarios @hdmi desta pasta ainda precisam de ser corridos na mao

  @critico @hdmi
  Cenario: Checklist minima para publicar
    Dado que a suite automatica passou
    E o CFG do lobby (F12) persistiu Salvar e as cenas no preview
    E o Ctrl+M em 1x e 1,5x mudou NR e Cinematic com reinicio de volta na sessao
    E o HDMI esta estavel
    Entao a build pode ir para o instalador
