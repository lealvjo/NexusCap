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

  @importante @hdmi
  Cenario: Overlay sem fundo liga e desliga sem reiniciar
    Dado que o CFG da sessao esta aberto
    Entao Overlay sem fundo esta sempre clicavel
    Quando eu marco ou desmarco Overlay sem fundo
    Entao compacto, mini, ADV e live perdem ou ganham a placa preta na hora
    E o processo nao reinicia

  @critico @hdmi
  Esquema do Cenario: NR on/off em qualquer Pass Count reinicia e volta na sessao
    Dado que o Pass Count atual e <modo>
    E o Neural Rendering esta <estado_inicial>
    Quando eu ligo ou desligo Neural Rendering ON no CFG (ou aperto Delete)
    Entao aparece o aviso Otimizando o pipeline cerca de 0,55 s
    E o overlay da sessao volta igual (nao cai no default do lobby)
    E o aviso de HDMI do console nao aparece
    E o processo volta para a sessao com --autostart (nunca para o lobby)
    E a imagem e o check do NR ficam iguais a <estado_final>

    Exemplos:
      | modo | estado_inicial | estado_final |
      | 1x   | ON             | OFF          |
      | 1x   | OFF            | ON           |
      | 1,5x | ON             | OFF          |
      | 1,5x | OFF            | ON           |
      | 2x   | ON             | OFF          |
      | 2x   | OFF            | ON           |
      | 3x   | ON             | OFF          |
      | 3x   | OFF            | ON           |

  @critico @hdmi
  Esquema do Cenario: Preset visual grava em todas as secoes e muda a imagem
    Dado que o Pass Count atual e <modo>
    E o preset visual atual e Default
    Quando eu escolho <cena> no CFG da sessao
    Entao o app reinicia e volta para a sessao
    E o aviso de HDMI do console nao aparece
    E a imagem fica diferente de Default (Cinema vs Default e o minimo)
    E ReShade.ini na secao [RenoDX.DLSS5] tem NRPreset e NRStyle da <cena>
    E se <modo> for 1,5x ou 2x ou 3x o DirectNeuralRenderingStyle tambem bate
    E o dlss5-feed.cfg tem preset= da <cena>

    Exemplos:
      | modo | cena       |
      | 1x   | Natural    |
      | 1x   | Cinematic  |
      | 1,5x | Model B    |
      | 1,5x | Model C    |
      | 2x   | Model B    |
      | 2x   | Model C    |
      | 3x   | Model B    |
      | 3x   | Model C    |

  @critico @hdmi
  Esquema do Cenario: Trocar Pass Count reinicia e volta na sessao
    Dado que o Pass Count atual e <de>
    Quando eu escolho <para> no CFG da sessao
    Entao aparece o aviso de motor ou de Pass Count
    E o aviso de HDMI do console nao aparece
    E o processo volta para a sessao (nao para o lobby)
    E o HUD mostra <para>
    E o AddonPath e o DirectNeuralRenderingPassCount batem com <para>

    Exemplos:
      | de   | para |
      | 1x   | 1,5x |
      | 1,5x | 2x   |
      | 2x   | 3x   |
      | 3x   | 1x   |
      | 1x   | 2x   |
      | 1,5x | 1x   |

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
    E o aviso de HDMI do console nao aparece
    E o processo volta para a sessao
    E o texto Trabalho Feed/NR mostra a nova resolucao efetiva (piso 50%)

  @importante @hdmi
  Cenario: Troca de jogo mantem o pino HDMI aberto
    Dado que a sessao captura o modo NV12 escolhido no lobby (nao um 2560x1440 travado)
    Quando eu entro ou saio de um jogo no console
    Entao o ecran fica no ultimo frame bom se o handshake sujar (nao preto nem magenta)
    E o aviso de cartao HDMI NAO aparece nessa troca
    E a captura NAO faz Stop/Run — o pino fica aberto
    E o primeiro frame bom volta ao Present logo
    E esse aviso so aparece se o HDMI cair de verdade (~2,5 s sem sinal / cabo)
    E esse aviso nao aparece em Delete, preset, Pass Count, escala do Feed nem no --autostart
    E frames rosa/magenta do handshake nao vao ao Present
    E buffers cujo tamanho nao e o modo aberto sao deitados fora e nao vao ao Neural Rendering
    E se o pin ficar no tamanho errado varios segundos (mudanca real de modo), a sessao segue esse modo

  @critico @auto
  Cenario: Selftest do CFG so confirma que o menu desenha
    # ctest -R selftest_cfg — nao substitui os cenarios HDMI acima
    Quando eu corro NexusCap --selftest-cfg
    Entao o ImGui do CFG inicializa e apresenta frames
    Mas isto nao valida clique em Cinema nem a imagem

  @critico @auto
  Cenario: Contrato dos presets de sessao sem janela
    # ctest -R unit — nome, snapshot Ctrl+M, upsert, import/export, apply-plan, disco temp
    Dado o contrato user_preset (nome 1-60, lista 32, AppData presets)
    Entao nome vazio ou so espaco nao salva
    E trim de "  TLOU  " vira TLOU
    E 61 caracteres recusa; 60 aceita
    E TLOU e tlou sao o mesmo preset
    E caracteres de arquivo (\ / : * ? " < > |) recusam na hora de salvar
    E o 33o nome novo recusa sem apagar os outros
    E o snapshot redonda HUD, fundo, console, NR, look, Pass, FG+xN e escala
    E Pass e FG ficam independentes no parse
    E look 2 no 1x e Cinematic e no 2x e Model C — o numero gravado nao traduz sozinho
    E o segundo save com o mesmo nome substitui (count 1)
    E apagar some da lista e do disco
    E o resumo da linha e "2x+  FG x3  100%" / "1x  FG off  100%" / "1.5x  FG x2  100%"
    E export importado volta igual; campo extra e ignorado; lixo deixa a lista intacta
    E pass 99 / scale 3 / fg 7 fazem clamp do produto
    E aplicar gera as mesmas SessionCfgActions do Ctrl+M (fill-safe)
    E 1x para 2x otimizado pede relaunch de motor; 2x default para 2x+ nao
    E FG no preset sem Lossless aplica o resto e deixa FG off
    E aplicar nao escreve device_name, hotkeys nem fg_path
    E --perf-test / Benchmark recusa load e save

  @critico @hdmi
  Cenario: Salvar e aplicar TLOU 2x devolve Pass e FG
    Dado que o CFG da sessao esta aberto
    E eu deixo 2x otimizado, look cinema/Model, Frame Gen x3 e escala 100
    Quando eu salvo o preset com o nome TLOU 2x
    Entao a lista mostra TLOU 2x e o resumo 2x+  FG x3  100%
    Quando eu fecho o Ctrl+M
    E mudo o Pass Count para 1x
    E abro o Ctrl+M de novo
    E seleciono TLOU 2x e clico Aplicar
    Entao a sessao volta para 2x otimizado e FG x3
    E o pipeline reinicia se o motor mudar
    E a sessao continua (nao volta ao lobby)

  @critico @hdmi
  Cenario: Salvar de novo o mesmo nome substitui a linha
    Dado que a lista ja tem TLOU 2x
    Quando eu salvo de novo com o nome TLOU 2x
    Entao aparece a confirmacao Substituir TLOU 2x?
    E depois so existe uma linha com aquele nome
    E o conteudo e o look atual do Ctrl+M

  @critico @hdmi
  Cenario: Compartilhar abre o Explorer no arquivo e Importar nao aplica sozinho
    Dado que uma linha de preset esta marcada
    Quando eu clico Compartilhar
    Entao o dialogo Salvar como abre na Area de trabalho
    E o nome sugerido termina em .nexuscap-preset
    E depois do OK o Explorer abre com o arquivo selecionado
    Quando no outro lado eu clico Importar
    Entao o dialogo Abrir arquivo filtra Preset NexusCap
    E a linha entra na lista (com confirmacao se o nome ja existir)
    E o preset ainda nao aplica — so depois de clicar a linha e Aplicar

  @importante @hdmi
  Cenario: Aplicar preset com FG sem Lossless deixa o resto e avisa
    Dado que o Lossless Scaling nao esta instalado
    E um preset tem Frame Gen ligado
    Quando eu aplico esse preset
    Entao HUD, look, Pass e escala aplicam
    E o Frame Gen fica off
    E aparece o aviso de Lossless nao encontrado

  @critico @hdmi
  Cenario: Benchmark nao le nem grava preset
    Dado que eu corro o Benchmark de 40 s
    Entao o bloco Presets do Ctrl+M fica bloqueado
    E nenhum arquivo novo aparece em %LOCALAPPDATA%\NexusCap\presets
    E o settings.ini do HDMI nao ganha knobs do teste
