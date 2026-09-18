# language: pt
Funcionalidade: CFG na captura (Ctrl+M)
  Como jogador
  Quero ajustar NR, cena, Pass Count, Frame Gen e escala por cima do jogo
  Para nao voltar ao lobby so para mudar uma opcao

  Contexto:
    Dado que o NexusCap esta na sessao de captura com RenoDX ligado
    E o HDMI (ou fake-capture nos selftests) mostra imagem

  @critico @hdmi
  Cenario: Abrir e fechar o CFG na frente do jogo
    Quando eu aperto Ctrl+M
    Entao o menu Configuracoes abre por cima da captura
    E o jogo continua no mesmo sinal
    Quando eu aperto Esc ou clico Fechar
    Entao o menu fecha
    E a sessao continua (nao volta ao lobby)

  @critico @hdmi
  Cenario: Segundo Esc depois de fechar o CFG volta ao lobby
    Dado que o CFG da sessao esta fechado
    Quando eu aperto Esc
    Entao o app volta ao lobby
    E nao fecha o processo

  @critico @atalho @hdmi
  Cenario: Com o CFG aberto as teclas da sessao nao disparam
    Dado que o CFG da sessao esta aberto
    Quando eu aperto Delete ou Page Down ou F11
    Entao o NR, o Frame Gen e a tela cheia nao mudam
    E so o menu consome o teclado

  @importante @hdmi
  Cenario: Overlay HUD liga e desliga sem reiniciar
    Dado que o CFG da sessao esta aberto
    Quando eu marco ou desmarco Overlay HUD
    Entao o overlay aparece ou some na hora
    E o processo nao reinicia

  @critico @hdmi
  Esquema do Cenario: NR on/off no 1x e ao vivo (sem restart)
    Dado que o Pass Count atual e 1x
    E o Neural Rendering esta <estado_inicial>
    Quando eu ligo ou desligo Neural Rendering ON no CFG (ou aperto Delete)
    Entao o processo nao reinicia
    E o ReShade.log ganha uma linha NR toggled <log>
    E o overlay Home nao abre
    E a imagem e o check do NR ficam iguais a <estado_final>
    E se o log nao confirmar o check volta ao <estado_inicial>

    Exemplos:
      | estado_inicial | estado_final | log |
      | ON             | OFF          | OFF |
      | OFF            | ON           | ON  |

  @critico @hdmi
  Esquema do Cenario: NR on/off no multipass reinicia na hora
    Dado que o Pass Count atual e <modo>
    E o Neural Rendering esta <estado_inicial>
    Quando eu ligo ou desligo Neural Rendering ON no CFG (ou aperto Delete)
    Entao aparece o aviso Reiniciando o NexusCap cerca de 1,1 s
    E o processo volta para a sessao com --autostart (nunca para o lobby)
    E o overlay Home nao abre
    E a imagem e o check do NR ficam iguais a <estado_final>

    Exemplos:
      | modo | estado_inicial | estado_final |
      | 1,5x | ON             | OFF          |
      | 1,5x | OFF            | ON           |
      | 2x   | ON             | OFF          |
      | 2x   | OFF            | ON           |
      | 3x   | ON             | OFF          |
      | 3x   | OFF            | ON           |

  @critico @hdmi
  Esquema do Cenario: Preset visual no 1x e ao vivo
    Dado que o Pass Count atual e 1x
    E o preset visual atual e Default
    Quando eu escolho <cena> no CFG da sessao
    Entao o app nao reinicia
    E a imagem fica diferente de Default (Cinema vs Default e o minimo)
    E ReShade.ini na secao [RenoDX.DLSS5] tem NRPreset e NRStyle da <cena>
    E o dlss5-feed.cfg tem preset= da <cena>
    E o overlay Home nao abre

    Exemplos:
      | cena       |
      | Natural    |
      | Cinematic  |

  @critico @hdmi
  Esquema do Cenario: Preset visual no multipass ao vivo (F6)
    Dado que o Pass Count atual e <modo>
    E o preset visual atual e Default
    Quando eu escolho <cena> no CFG da sessao
    Entao o overlay Home nao abre
    E o ReShade.log ganha committed preset transaction
    E o app nao reinicia
    E a imagem fica diferente de Default (Cinema vs Default e o minimo)
    E ReShade.ini na secao [RenoDX.DLSS5] tem NRPreset e NRStyle da <cena>
    E o DirectNeuralRenderingStyle tambem bate
    E o [RenoDXNeuralResolution] tem PresetCycleKey=117
    E o dlss5-feed.cfg tem preset= da <cena>

    Exemplos:
      | modo | cena       |
      | 1,5x | Natural    |
      | 1,5x | Cinematic  |
      | 2x   | Natural    |
      | 2x   | Cinematic  |
      | 3x   | Natural    |
      | 3x   | Cinematic  |

  @critico @hdmi
  Esquema do Cenario: Trocar 1x com outro x reinicia o motor
    Dado que o Pass Count atual e <de>
    Quando eu escolho <para> no CFG da sessao
    Entao aparece o aviso de motor
    E o processo volta para a sessao (nao para o lobby)
    E o HUD mostra <para>
    E o AddonPath bate com <para>

    Exemplos:
      | de   | para |
      | 1x   | 1,5x |
      | 3x   | 1x   |
      | 1x   | 2x   |
      | 1,5x | 1x   |

  @critico @hdmi
  Esquema do Cenario: 1,5x 2x 3x ao vivo com mais e menos
    Dado que o Pass Count atual e <de>
    Quando eu escolho <para> no CFG da sessao
    Entao o overlay Home nao abre
    E o ReShade.log ganha pass-count hotkey set
    E o processo nao reinicia
    E o HUD mostra <para>

    Exemplos:
      | de   | para |
      | 1,5x | 2x   |
      | 2x   | 3x   |

  @importante @hdmi
  Cenario: Frame Gen no CFG da sessao entra ao vivo
    Dado que o Lossless Scaling esta instalado
    E o CFG da sessao esta aberto
    Quando eu habilito Frame Gen e escolho x2 ou x3 ou x4
    Entao o FG muda sem reiniciar o NexusCap
    E o HUD mostra o multiplicador

  @importante @hdmi
  Cenario: Aplicar escala do Feed reinicia se o Feed ja travou
    Dado que a sessao esta com RenoDX e o Feed travou em 100%
    Quando eu movo o slider e clico Aplicar escala (reinicio)
    Entao o aviso de escala aparece
    E o processo volta para a sessao
    E o texto Trabalho Feed/NR mostra a nova resolucao efetiva (piso 50%)

  @critico @auto
  Cenario: Selftest do CFG so confirma que o menu desenha
    # ctest -R selftest_cfg — nao substitui os cenarios HDMI acima
    Quando eu corro NexusCap --selftest-cfg
    Entao o ImGui do CFG inicializa e apresenta frames
    Mas isto nao valida clique em Cinema nem a imagem
