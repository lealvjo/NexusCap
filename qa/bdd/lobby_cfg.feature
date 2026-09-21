# language: pt
Funcionalidade: CFG do menu principal
  Como utilizador
  Quero ajustar FG, escala, RenoDX, cenas, Pass Count e atalhos no lobby
  Vendo o preview ao vivo — isto nao e o Ctrl+M

  Contexto:
    Dado que estou no lobby
    E o painel Configuracoes (CFG) esta aberto (botao CFG ou F12)
    E o preview continua visivel ao lado

  @critico @hdmi
  Cenario: Fechar e Salvar persistem
    Quando eu mudo uma opcao e clico Salvar ou Fechar
    Entao o painel fecha
    E %LOCALAPPDATA%\NexusCap\settings.ini tem os valores novos
    E os ficheiros RenoDX ao lado do exe batem com o preset e o Pass Count
    Quando eu fecho o app e abro de novo
    Entao as mesmas opcoes voltam

  @critico @hdmi
  Cenario: Esc fecha o CFG do lobby e nao o app
    Quando eu aperto Esc
    Entao o painel CFG fecha
    E o lobby continua aberto

  @importante @hdmi
  Cenario: Frame Gen no lobby com Lossless
    Dado que o Lossless Scaling esta na Steam ou no caminho Auto
    Quando eu habilito Frame Gen e escolho x2 ou x3 ou x4
    Entao o HUD do preview muda na hora
    E nao reinicia o app

  @importante @hdmi
  Cenario: Frame Gen sem Lossless volta off
    Dado que o LosslessScaling.exe nao existe
    Quando eu marco Habilitar Frame Gen
    Entao o check volta off
    E aparece o toast de Lossless nao encontrado

  @importante @hdmi
  Cenario: Pasta e Auto do Frame Gen
    Quando eu clico Pasta e escolho LosslessScaling.exe
    Entao o caminho manual fica visivel e OK
    E a config anti-ghost da mira (Flow 100 / WGC) continua
    Quando eu clico Auto
    Entao o caminho manual some
    E o texto mostra auto se o exe for achado
    E a mira-lock continua igual

  @importante @hdmi
  Cenario: Mira-lock sempre no Frame Gen
    Dado instalacao do zero, Auto Steam ou Pasta manual
    Quando o NexusCap aplica o perfil LSFG
    Entao LSFGFlowScale e 100, CaptureApi e WGC, sem Flow 70

  @importante @hdmi
  Cenario: Escala no lobby nao aplica no Feed deste processo
    Dado que o RenoDX esta ligado e o Feed do lobby travou em 100%
    Quando eu movo o slider da escala
    Entao o texto Trabalho Feed/NR atualiza
    E o preview do lobby continua nitido (100%)
    E se a escala for abaixo de 50% o aviso do piso aparece
    Quando eu clico Iniciar com escala diferente do Feed travado
    Entao o app reinicia e a sessao entra com a escala nova

  @importante @hdmi
  Cenario: Abrir logs
    Quando eu clico Abrir logs
    Entao a pasta de logs do app abre
    Ou aparece toast se nao der para abrir

  @critico @hdmi
  Cenario: RenoDX no preview liga e desliga na hora
    Quando eu marco RenoDX / Neural Rendering (preview ao vivo)
    Entao o preview ganha os filtros
    E o toast diz Neural Rendering ON
    Quando eu desmarco
    Entao o preview fica limpo
    E o toast diz Neural Rendering OFF
    E Delete na captura so funciona depois de marcar de novo

  @critico @hdmi
  Esquema do Cenario: Preset visual no preview (1x ao vivo, multipass no Iniciar)
    Dado que o RenoDX esta ligado
    E o Pass Count do lobby e <modo>
    Quando eu escolho <cena>
    Entao no 1x o preview muda na hora
    E no 1,5x ou 2x ou 3x o texto avisa que o Iniciar aplica o preset
    Quando eu inicio a sessao
    Entao o look da captura e o da <cena>

    Exemplos:
      | modo | cena       |
      | 1x   | Default    |
      | 1x   | Natural    |
      | 1x   | Cinematic  |
      | 1,5x | Model C    |
      | 2x   | Model B    |
      | 3x   | Model A    |

  @critico @hdmi
  Esquema do Cenario: Pass Count no lobby reinicia o app no lobby
    Dado que o RenoDX esta ligado
    E o Pass Count atual e <de>
    Quando eu escolho <para>
    Entao o app grava settings e os ficheiros
    E aparece o aviso de motor ou Pass Count cerca de 0,55 s
    E o processo reabre no lobby (nao na sessao)
    E o painel CFG ja esta aberto (estava aberto no ajuste)
    E o radio fica em <para>

    Exemplos:
      | de   | para |
      | 1x   | 1,5x |
      | 1,5x | 2x   |
      | 2x   | 3x   |
      | 3x   | 1x   |

  @critico @hdmi
  Esquema do Cenario: Preset visual e escala 100%/50% nao se pisam
    Dado que o RenoDX esta ligado
    E o Pass Count do lobby e <modo>
    E a escala e <escala>
    Quando eu escolho <cena>
    Entao o dlss5-feed.cfg tem o preset da <cena> e work_resolution=<escala>
    E o overlay mostra a <cena> e a escala <escala>
    E mudar a escala nao volta o look para Default/Model A

    Exemplos:
      | modo | cena       | escala |
      | 1x   | Cinematic  | 100    |
      | 1x   | Natural    | 50     |
      | 1,5x | Model C    | 50     |
      | 1,5x | Model A    | 100    |
      | 2x   | Model B    | 50     |
      | 3x   | Model C    | 100    |

  @importante @atalho @hdmi
  Cenario: Rebind de atalhos no CFG do lobby
    Quando eu clico no botao de Overlay HUD e aperto outra tecla
    Entao o rotulo muda para essa tecla
    E o check Ctrl do HUD continua a valer
    Quando eu clico no botao de Menu na captura e mudo a tecla ou o Ctrl
    Entao o Ctrl+M (ou o novo atalho) e o que abre o CFG na sessao
    Quando eu aperto Esc enquanto "pressione tecla" esta ativo
    Entao a captura de tecla cancela
    E o painel CFG nao fecha por esse Esc

  @critico @hdmi
  Cenario: Lobby CFG e Ctrl+M nao se comportam igual
    Dado que mudei cena e NR no CFG do lobby
    Entao o preview mudou sem reiniciar a sessao
    Quando eu inicio e abro Ctrl+M
    E mudo cena ou NR ou Pass Count
    Entao a sessao reinicia e volta para a captura

  @critico @auto
  Cenario: Contrato lobby dos presets sem janela
    # ctest -R unit — from_persisted, apply-plan, FG missing, sem tocar placa
    Dado um snapshot do CFG do lobby (HUD, Pass, look, FG, escala)
    Entao user_preset_from_persisted redonda os mesmos campos do Ctrl+M
    E aplicar no PersistedSettings nao escreve device_name, hotkeys nem fg_path
    E 1x para 2x otimizado pede relaunch de motor no lobby
    E FG no preset sem Lossless aplica o resto e deixa FG off

  @critico @hdmi
  Cenario: Presets no lobby usam a mesma lista do Ctrl+M
    Dado que estou no lobby
    Entao a coluna SESSAO nao mostra presets
    Quando eu abro o CFG (F12)
    Entao o bloco Presets tem Salvar, Aplicar, Apagar, Compartilhar e Importar
    E a lista e a mesma pasta %LOCALAPPDATA%\NexusCap\presets
    Quando eu aplico TLOU 2x no lobby
    Entao FG, Pass, look e escala do CFG mudam
    E o settings.ini grava
    E o Iniciar abre nesse jeito
    E Importar nao aplica sozinho

  @importante @hdmi
  Cenario: Aplicar preset no lobby com troca de motor reinicia o menu
    Dado que o preview CFG ao vivo esta ligado
    E o Pass atual e 1x
    Quando eu aplico um preset 2x otimizado
    Entao aparece o hold do pipeline
    E o lobby volta com o CFG se estava aberto
    E a sessao ainda nao comecou
