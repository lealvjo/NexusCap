# language: pt
Funcionalidade: Atalhos
  Como jogador
  Quero os atalhos default e os que eu rebindar no CFG do lobby
  Sem o menu da captura roubar as teclas quando estiver aberto

  @critico @atalho @hdmi
  Esquema do Cenario: Defaults na sessao
    Dado que os atalhos estao no default de fabrica
    E estou na sessao de captura com o CFG Ctrl+M fechado
    Quando eu aperto <tecla>
    Entao acontece <efeito>

    Exemplos:
      | tecla        | efeito                                      |
      | Esc          | volta ao lobby (CFG fechado)                |
      | Ctrl+Espaco  | mostra ou esconde o overlay HUD             |
      | Delete       | liga ou desliga NR e reinicia a sessao      |
      | Page Down    | liga ou desliga Frame Gen                   |
      | Page Up      | cicla Frame Gen x2 / x3 / x4                |
      | Home         | overlay / presets do RenoDX                 |
      | F11          | tela cheia ou janela                        |
      | F4           | analyze                                     |
      | Ctrl+M       | abre o CFG da sessao                        |

  @critico @atalho @hdmi
  Cenario: F12 abre o CFG no lobby
    Dado que estou no lobby
    Quando eu aperto F12
    Entao o painel Configuracoes (CFG) do menu principal abre

  @critico @atalho @hdmi
  Cenario: Esc com CFG da sessao aberto so fecha o menu
    Dado que o CFG da sessao (Ctrl+M) esta aberto
    Quando eu aperto Esc
    Entao o menu fecha
    E a sessao continua
    Quando eu aperto Esc de novo
    Entao volto ao lobby

  @critico @atalho @hdmi
  Cenario: Com Ctrl+M aberto Delete PageDn e F11 nao disparam
    Dado que o CFG da sessao esta aberto
    Quando eu aperto Delete ou Page Down ou F11
    Entao NR, Frame Gen e tela cheia ficam como estavam

  @critico @hdmi
  Cenario: Delete sem RenoDX nao finge HUD off
    Dado que RenoDX nao esta carregado nesta sessao
    Quando eu aperto Delete
    Entao aparece aviso para marcar RenoDX no lobby
    E o HUD nao mostra NR off com a imagem ainda ligada

  @importante @atalho @hdmi
  Cenario: Rebind persiste
    Dado que no CFG do lobby eu mudei Overlay HUD e Menu na captura
    Quando eu salvo, fecho o app e abro de novo
    Entao as teclas novas e os checks Ctrl continuam
    E na sessao so esses atalhos abrem HUD e CFG
