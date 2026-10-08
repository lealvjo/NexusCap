# NexusCap

<p align="center">
  <img src="assets/logo.png" alt="NexusCap" width="160" />
</p>

**Viewer nativo de captura HDMI** — sinal direto da placa (DirectShow), **DLSS 5 / Neural Rendering** no Present (já no setup), Frame Gen opcional (**Lossless Scaling**), lobby com preview e CFG.

| | |
|---|---|
| **Versão** | `6.6.2` |
| **Repo** | privado — https://github.com/lealvjo/NexusCap |
| **Linguagem** | **C++17** (nativo Windows) |
| **Stack** | CMake · D3D11 · DirectShow · ImGui · WASAPI |
| **Settings / install** | Setup: `%LOCALAPPDATA%\NexusCap` · Portable: pasta do zip (`portable.txt`) |

---

## Para a comunidade (download)

Distribua o **instalador** ou o **zip portable** — sem pasta manual, sem código:

**[NexusCap-Setup-6.6.2.exe](https://github.com/lealvjo/NexusCap/releases/tag/v6.6.2)** — instala na conta, sem admin.

**[NexusCap-Portable-6.6.2.zip](https://github.com/lealvjo/NexusCap/releases/tag/v6.6.2)** — extrai e roda. Settings e presets viajam com a pasta.

O setup e o zip **não ficam no git**; sobem na **Release** do GitHub.

**O que o usuário final faz:**

1. **Setup:** instalar `NexusCap-Setup-6.6.2.exe` (vai para `%LOCALAPPDATA%\NexusCap`, **sem admin**).
   **Portable:** extrair `NexusCap-Portable-6.6.2.zip` numa pasta gravável (SSD; não use Program Files).
2. Abrir o app e usar — **RenoDX / Neural Rendering / mini overlay / VSYNC / métricas 2.x já vêm no pacote**.
3. **Só se quiser Frame Gen:** instalar [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) na Steam (não vem no setup nem no zip). Page Down no CFG.

Nada mais. Sem copiar DLL avulsa, sem pasta X2.

### Requisitos do usuário final

| Precisa | Detalhe |
|---------|---------|
| **Placa de captura** | HDMI via DirectShow (ex.: VEDO / Elgato / similar) |
| **GPU NVIDIA** | Para RenoDX / Neural Rendering (já embutido no setup e no zip) |
| **Windows 64-bit** | Build nativo |
| **Frame Gen (opcional)** | [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) na Steam — **única** instalação extra |

### Instalador menos invasivo

O setup **não pede administrador** e **não mostra** o UAC (“Deseja permitir que este aplicativo faça alterações no dispositivo?”). Instala na pasta da sua conta — `%LOCALAPPDATA%\NexusCap` — no mesmo espírito do Steam, Discord e Spotify.

Isso **não é menos seguro**. Aquele aviso só aparece quando o programa quer poder no sistema inteiro (`Program Files`, serviços, outras contas). Nós não precisamos disso. Pedir admin só para parecer “oficial” seria **mais perigoso**.

RenoDX / ReShade precisam gravar `ini`, `cfg` e cache **ao lado do exe**. Em Program Files o Windows bloqueia a escrita; presets falham. Por isso o padrão é a pasta do usuario: menos invasivo, e o enhance funciona.

### Passo a passo de instalação

1. Baixe `NexusCap-Setup-6.6.2.exe` **ou** `NexusCap-Portable-6.6.2.zip` na Release do GitHub.
2. Setup: execute (instala em `%LOCALAPPDATA%\NexusCap`, sem admin). Portable: extraia numa pasta gravável e abra `NexusCap.exe` — o `portable.txt` faz settings/presets/logs ficarem nessa pasta.
3. Abra **Nexus Cap**, escolha a placa de captura e ajuste no lobby (preview ao vivo + áudio HDMI).
4. Marque **RenoDX** se quiser Neural Rendering (Delete liga/desliga na captura). Com RenoDX, a escala do CFG afeta só o Feed (Present fica nativo — melhor nitidez).
5. **Frame Gen (opcional):** instale Lossless Scaling na Steam; no CFG, Page Down liga/desliga. Se o exe não for achado, use **Pasta** e aponte `LosslessScaling.exe`.
6. **Iniciar** — Esc volta ao lobby. **F12** (ou o botão CFG) abre o painel do menu principal. **Ctrl+M** abre o CFG por cima do jogo (ajusta sem sair da captura) e **Ctrl+Espaço** cicla os overlays HUD (Compact → Mini → Advanced → Live → desliga; o próximo toque recomeça). Os atalhos são configuráveis. Use **Abrir logs** no CFG se precisar de suporte.
7. **Reportar bug / Reportar performance** (checkboxes abaixo do VSYNC, off por padrão): geram `.txt` leves em `report/` e `performance/` ao lado do exe — úteis para suporte / colar numa IA.

**Evite instalar em Program Files** — o Present/RenoDX precisa gravar ao lado do exe.

### Presets

Montou o jogo. Salva. Acabou a dancinha de Pass, cena e Frame Gen toda vez que abre o HDMI.

**Ctrl+M** na captura ou **F12** no lobby — a mesma lista. Um nome (até 50). Aplicar devolve o look inteiro: escala, FG, NR, cena, Pass, HUD.

O amigo manda um `.nexuscap-preset`. Importar entra na lista. Aplicar é você quem manda.

### Novidades 6.6.2

- A Janela entrega BGRA direto na GPU, sem passar por NV12. O overlay mostra BGRA. O preview do lobby fica no placeholder ate iniciar.

### Novidades 6.6.1

- Desempenho maximo fica ligado por padrao. Pede a NVIDIA para preferir o clock maximo so neste programa e tira o processo do power throttling do Windows. Sem placa NVIDIA, so a parte da CPU vale.
- Estabilidade de detalhes saiu do bloco fechado e ficou no config, ao lado do Pass Count. Auto, Desligado ou Sempre, com o que cada um faz no menu.

### Novidades 6.6.0

- O Ctrl+M mostra Informacoes rapidas: GPU, CPU, VRAM e watts da placa. Uso baixo fica verde e uso alto fica vermelho.
- No modo Janela, o preview sem imagem deixa de falar da placa e deixa de ficar preto.
- O lobby avisa que Ctrl+M abre o menu ao vivo e que Ctrl+Espaco troca o overlay.
- Shift++ e Shift+- trocam Default, Natural e Cinematic. O quadro fica parado. A tecla e configuravel no lobby e no Ctrl+M.

### Novidades 6.5.8

- MJPEG passa a entregar video. O JPEG da placa vira o mesmo NV12 do resto do app, e o overlay mostra MJPEG. NV12 continua o caminho de sempre.
- O mini overlay quebra a linha quando o texto nao cabe, para nada sumir ao lado do grafico.
- Gamer sobe o agudo e Graves sobe o grave no audio da captura. Padrao continua plano.

### Novidades 6.5.7

- Quando a placa nao entrega video na resolucao e no Hz escolhidos, o aviso diz isso e que essa configuracao nao tem suporte.
- Esse Hz fica no menu como sem suporte e nao pode ser clicado. Iniciar processamento fica desligado enquanto essa configuracao estiver selecionada.
- A frase de placa em uso continua so quando o USB esta ocupado.

### Novidades 6.5.6

- A resolucao, o Hz e o formato seguem a placa selecionada. Trocar a placa e recarregar as entradas monta a lista dela. 1440p volta a oferecer 30 quando a faixa da placa cobre 30.
- Se o modo nao abre, o aviso diz a resolucao e o Hz. A frase de placa em uso fica para quando o USB esta ocupado.
- O CFG do lobby acompanha o Ctrl+M. Home abre o RenoDX. O som tem Padrao, Gamer e Graves.

### Novidades 6.5.5

- No Ctrl+M, Cinematic, Pass Count, escala, Neural Rendering e preset seguram o frame da placa. O atalho Delete e o Ctrl+ / Ctrl- fazem o mesmo, sem apresentar um frame no meio.
- Menu e atalho trocam o X com a capa no vblank. O frame fica inteiro enquanto o scanout sai.
- O ponteiro não troca de tamanho nem fica na bolinha de carregamento enquanto o motor reinicia.

### Novidades 6.5.4

- Trocar o X, pelo atalho ou pelo Ctrl+M, não abre mais o que está atrás. A capa segura o último frame enquanto o scanout sai, inclusive no 3x.

### Novidades 6.5.3

- No 2x e no 3x, o gráfico de frametime e os números do overlay (fps, ft, média) seguem a média do ritmo. As pirâmides somem e o 60 deixa de piscar. No 1x, um tick pulado continua aparecendo.
- Trocar o X não abre mais um flash do que está atrás. A capa do último frame fica na tela enquanto o scanout sai.

### Novidades 6.5.2

- Depois de Iniciar, o lobby avisa que Ctrl+M abre o menu de configurações.
- Compacto, mini e métricas avançadas mostram o formato da captura (NV12, MJPEG, YUY2, I420). O overlay live não mostra.
- Os contadores `drop` e `frm` saíram da linha de stalls e clusters.
- Os avisos aparecem no canto inferior esquerdo.

### Novidades 6.5.1

- O menu DLSS 5 Neural Rendering abre em 895×1296, o mesmo tamanho do overlay de Complementos e Início.

### Novidades 6.5.0

- Mini, painel e live dizem **DLSS5 ON** ou **DLSS5 off**, junto com o nome do X.
- Iniciar da janela solta a placa antes e entra sem o cartão "Otimizando pipeline". Troca de X, preset ou NR na janela segura o último frame. O HDMI fica como estava.
- O FPS do overlay segue o período da captura. Um tick pulado não puxa o número para baixo.
- 2x default e 3x default saíram. Ficam 1x, 1,5x, 2x otimizado e 3x otimizado.
- Os modos da placa vêm do intervalo que ela mesma publica. Resolução e Hz ficam em menus separados. NV12 entra sempre que essa combinação tiver NV12.
- O benchmark usa os mesmos menus de Resolução e Hz. O painel da sessão está em Overlay, Imagem e Relatório.

### Novidades 6.0.4

- **Ctrl++** e **Ctrl+-** passam o X (1x até 3x otimizado, com volta). Estão em Atalhos e dá para gravar outra tecla. O numpad acompanha o +/−.
- O gráfico de frametime do mini overlay ocupa a sobra da barra, em vez do bloco pequeno no canto.
- Aplicar um preset mostra **Aplicando preset → nome**.
- Configurações avançadas ficam no X escolhido. Fechar sem Aplicar pergunta. Aplicar volta com o menu aberto.
- O aviso de Home do RenoDX não aparece ao ligar o Neural Rendering.

### Novidades 6.0.3

- Escala de fluxo no CFG e no overlay: **Padrão** (nosso valor), 70, 90 ou 100. Trocar reabre o Lossless Scaling.
- O look do Neural Rendering usa os nomes do mod: **Default**, **Natural** e **Cinematic**.

### Novidades 6.0.2

- Restart do motor: a imagem HDMI volta em cerca de 0,9 s. O aviso e o frame congelado continuam.
- O Lossless Scaling fica aberto na troca de X, preset, NR ou escala. O atalho volta depois do primeiro frame.
- Som e leitura de CPU/GPU também esperam esse frame, para não segurar a tela.

### Novidades 6.0.1

- Restart mais leve: cursor normal, hold responsivo, menos espera fixa e sem regravar configuração idêntica.
- Troca de X / NR / preset / escala **aplica o motor com a tela no sítio**. O aviso é “A aplicar o motor”; a imagem fica.
- Um GPU: captura, Present e ReShade 8.5 no mesmo processo. Sem segunda janela.
- O 8.5 só lê o X quando nasce — a sessão volta com `--autostart`, nunca no lobby.
- CFG em abas (F12 e Ctrl+M): **Imagem** (X, look, escala, presets) · **HUD/Som** · **Frame Gen** · **Atalhos**.

### Novidades 5.2.1

- **1,5x** mais perto do 2x em tom: sombras menos esmagadas, um pouco mais de cor, matiz ajustado. O detalhe do segundo passe continua so no 2x.

### Novidades 5.2.0

- Motor Neural Rendering mais novo (Krish 8.5). Overlay em português.
- **1x** é o antigo 1,5x. **1,5x** herda a receita de edição do 1,7x. O 1,7x saiu da lista.
- Quem tinha 1,7x gravado passa sozinho para 1,5x.

### Novidades 5.1.2

- Avisos de Pass Count, preset, escala e DLSS 5 voltam depois do restart (o processo antigo morria com o toast).
- Ligar/desligar DLSS 5 na captura tambem deixa o aviso na volta.
- Mensagens de pipeline/reinicio sem o travessao.

### Novidades 5.1.1

- Tabela do Benchmark: o 1,7x deixa de contar como 2x cheio. A nota e o conselho de escala/FG nao mudam.

### Novidades 5.1.0

- **Janela** no lobby, ao lado da placa HDMI. Captura qualquer janela via WGC e usa o mesmo Present, DLSS 1,5x/1,7x/2x/3x, overlays e Frame Gen.
- HDMI nao muda: default ao abrir, recover DShow, WASAPI, Flow 80, WGC do LS, F24, FIXED. Janela e opt-in.
- Janela/borderless (fullscreen exclusivo falha), DRM vem preto, minimizada para, audio fica no app de origem.

### Novidades 5.0.2

- Overlay extra GDI (painel HW separado) saiu. Compacto, Mini, ADV e Live continuam queimados no Present para o OBS.
- Toast de Frame Gen: o Present queima direto no centro, 3x, sem wrappers mortos.
- Frame Gen na sessao usa so request + tick (saiu o set/cycle sincrono que ninguem chamava).

### Novidades 5.0.1

- **Modo FG Adaptive** no lobby e no `Ctrl+M`: teto de FPS (piso = captura, teto = Hz do monitor), **Aplicar Adaptive**, Fixo vale na hora. Overlay mostra `Fixo x2` ou `Adaptive 70`.
- Adaptive gera frames até o que você definiu. Fixo continua x2/x3/x4. Default Fixo. No Benchmark o Iniciar já carrega o plano.
- Aviso de Frame Gen ON / xN: toast grande no **centro da tela** em todos os overlays, sem misturar com a faixa. Sem aviso de artefatos.
- Overlay live deixa de partir quando o toast aparece (textura própria). Analyze só com Reportar bug / Insert.
- Frametime igual em compacto, ADV e mini (`ft / avg / p95 / FG est`). `present` fica na latência. Live mostra last + FG est.
- Teste de contrato FG ON (F24 ou superfície → HUD).

### Novidades 5.0.0

- **Sem o chip de FPS do Lossless Scaling.** O hide grava o `config.ini`, lê de volta, e não trava o ficheiro (o `Save()` do LS morria com ReadOnly e o frametime ia com ele).
- O script antigo de mira (`keep-flow90`) saiu do arranque — era ele que religava o contador e gravava o INI a cada 150 ms.
- Frame Gen, Flow 80, WGC e F24 ficam como estavam.

### Novidades 4.1.8

- Os quatro overlays (Live / Mini / Compacto / ADV) entram no quadro do Present. OBS grava a janela e o HUD vai no vídeo.
- Live no mesmo DPI da tela, sem a faixa GDI por cima (não duplica).

### Novidades 4.1.7

- Overlay **Live:** sem modelo do motor; DLSS com o mesmo tag dos outros (`1x` / `1.5x` / `2x` / `2x+` / `3x` / `3x+`) e **FG x2 / x3 / x4 / FG off**.
- **PS5 Pro** no seletor de console; preview do Live no lobby.
- Contador de FPS nos quatro overlays (Mini / Compacto / ADV / Live) usa as cores do frametime (cap / x2 / x3 / x4).

### Novidades 4.1.6

- **1,5x** e **3x otimizado** usam o mesmo gancho do **2x otimizado** (anti-flicker / look). O motor continua 1 evaluate no 1,5x e 3 no 3x.
- **3x default** fica ao lado, para quem quiser comparar o look antigo.
- Presets continuam gravando HUD, overlay, console, NR, look, Pass (incluindo `3x+` / `2x+`), FG e escala.

### Novidades 4.1.5

- **Atalhos no CFG:** nome, tecla e checkbox Ctrl em colunas. O texto não fica atrás do botão nem cortado.

### Novidades 4.1.4

- **Volume HDMI** no CFG (lobby, sessão e Benchmark). Não entra no preset.
- **Home:** 1x abre o overlay ReShade com todas as abas; 2x/3x continua o painel RenoDX DLSS.
- HDMI `--autostart` volta a sincronizar o MGF (1x não arranca sem técnicas).

### Novidades 4.1.3

- **Mini overlay do lobby ao vivo:** CPU, GPU e temps sao da maquina, nao um i5/RTX inventado. A sessao ja era assim.

### Novidades 4.1.2

- **Setup e zip no mesmo release.** Instala na conta, ou extrai e leva a pasta. Presets e settings viajam com o `portable.txt`. Frame Gen continua nesta maquina.

### Novidades 4.1.1

- **Presets na pratica:** Renomear, nome ate 50, lista em duas linhas (cena + Pass + FG). Compartilha e importa `.nexuscap-preset`.
- Pitch e README falam disso. Print novo do 2x otimizado.

### Novidades 4.1.0

- **Presets de sessao:** o snapshot do Ctrl+M (escala, FG, NR, look, Pass, HUD/console) grava com nome de 1 a 50 caracteres em `%LOCALAPPDATA%\NexusCap\presets` (instalado) ou `presets\` ao lado do exe (portable). O mesmo bloco aparece no CFG do lobby (F12), nao na coluna SESSAO.
- **Aplicar / compartilhar / importar:** nome unico (case-insensitive, overwrite). Arquivo `.nexuscap-preset`. Importar nao aplica sozinho. Benchmark nao le nem grava preset.
- **Reportar performance no HDMI:** Esc com escala/NR relanca o lobby e ainda abre a analise (mesmo cartao do Benchmark).
- Traz o 4.0.1/4.0.2: overlay FG ON real, instalador sem video duplicado e sem NGX na raiz (~693 MB).

### Novidades 4.0.2

- **Instalador sem NGX na raiz:** `nvngx_dlss*.dll` fica so em `addons-stable` e `addons-multipass` (o Feed le a pasta do addon). Setup ~693 MB. 1x, 1,5x, 2x, 2x otimizado e 3x validados com a raiz vazia.

### Novidades 4.0.1

- **Overlay FG ON de verdade:** o HUD e o Benchmark marcam Frame Gen ligado quando o F24 confirma e o Lossless Scaling ainda esta vivo. A janela grande de geracao continua valendo, mas nao e mais obrigatoria (DPI e estilos TOPMOST/LAYERED nao escondem o ON).
- **Instalador mais leve:** sai o `perf_test.mp4` duplicado (era o mesmo clipe 1440). Ficam 1080 / 1440 / 4K + audio. Setup ~827 MB.

### Novidades 4.0.0

- **CFG no preview:** o menu abre em HDMI cru. Quem quiser o look ao vivo clica **Visualizar configuracoes no preview em tempo real** (relanca o pipeline). O Iniciar nao usa esta flag.
- **Aviso de motores:** se trocarem o Feed ou o RenoDX depois de instalar, o footer avisa (size+SHA uma vez ao abrir). Motores oficiais = sem mensagem.
- **2x otimizado:** mesmos dois evaluates do 2x default, look exclusivo (gancho de cor/UI mais estavel). Menos flicker, cintilacao e ghost no mesmo ~40 fps.
- **Overlay proprio do Nexus Cap:** HUD queimado no Present. Sem Afterburner nem overlay de terceiro.
- **Frametime com FG:** duas linhas (cap em baixo, FG em cima) com o FPS a esquerda. **STABLE** / **PLAYABLE FG** / **UNSTABLE**.
- **Benchmark no seu PC:** 40 s de The Last of Us Parte II, sem placa de captura. Modelo com fps, present, latencia, stalls, escala e FG. Cada cartao sugere o proximo passo, inclusive se a latencia extra do Frame Gen ainda da para jogar.
- **Reportar performance** no HDMI: no fim da captura abre o mesmo cartao do Benchmark.
- **Benchmark nao vaza no lobby:** CFG live off nao esvazia o pipeline do teste. Esc, X ou o timer devolvem escala, Pass, NR e Feed ao HDMI.
- **Validado em PS5, Switch 2 e Xbox Series S.** Os prints do pitch sao 2x otimizado + FG x3. 1x, 1,5x, 2x e 3x usam o mesmo Present.

### Novidades 3.1.11

- **2x otimizado** exclusivo NexusCap: os mesmos dois evaluates do 2x default, com gancho de cor/UI mais estável — menos flicker e ghost, melhor imagem. O 2x default fica como estava. 1x, 1,5x e 3x sem mudança.
- **Reportar performance** no HDMI: no Esc/X abre o mesmo cartão do Benchmark (nota + conselho). Título: *Reportar performance ao finalizar gameplay*. O Benchmark (clipe 40 s, hold, relançar) não muda.
- **Frametime com FG:** duas linhas (cap em baixo, FG em cima) com o FPS à esquerda. Overlay **STABLE** / **PLAYABLE FG** / **UNSTABLE** — dourado quando o FG cobre a meta 30/60.

### Novidades 3.1.10

- **Benchmark não vaza no lobby:** Esc, X ou o fim do teste devolvem escala, Pass, tom, NR e Feed ao HDMI. O Lossless Scaling é encerrado (não vai para a bandeja) e o app relança o lobby limpo.
- **Gráfico com FG off:** só a linha da captura. x2/x3/x4 só aparecem com Frame Gen ligado.

### Novidades 3.1.9

- **Benchmark no lobby:** quem ainda não tem placa corre um clipe no mesmo Present / NR / FG da sessão. O relógio só arranca quando o pipeline escolhido está mesmo ligado.
- **Pino virtual de captura:** cadência 30 ou 60, decode à frente, sem câmera lenta nem seek a chutar o quadro.
- **Conselho estatístico:** escala e Frame Gen saem do fps medido, sem tabela chumbada.
- HDMI real 1440p60 **1x e 1,5x** continuam a ~60 fps com 0 stalls nesta máquina.

### Novidades 3.1.8

- **Overlay no restart:** mudar motor / Pass Count / escala / NR volta para o mesmo overlay da sessão (mini, ADV, live…). O ciclo Ctrl+Espaço continua só na sessão — os checks do lobby não mudam.
- **Frame Gen no gráfico:** x2/x3/x4 desenham uma linha extra por multiplicador, cada uma com cor própria, acima do frametime da captura. Vale também se o console estiver abaixo de 60 fps.
- **Aviso de restart:** “Otimizando o pipeline” no hold do motor, em vez de parecer que o app fechou.

### Novidades 3.1.7

- **HDMI do console, sem susto:** se o Xbox/PS5 para de mandar sinal (entrar/sair do jogo), o Present segura o último frame bom — sem magenta, sem flicker. Um cartão dourado queimado no quadro explica que **foi o console** que cortou o HDMI; o NexusCap está só a limpar o pipeline para quando o sinal voltar.
- **Um cartão por handshake**, ~3 s, e some quando o sinal estabiliza. Delete, NR, preset, Pass Count, escala do Feed e `--autostart` **não** disparam este aviso — isso é restart nosso, não dropout do console. Também não entra toast extra no overlay.

### Novidades 3.1.6

- **HDMI Xbox / consola:** ao entrar ou sair do jogo o ecrã segura o último frame bom (não fica preto nem flicker), deita fora buffers do tamanho errado, e reabre a captura sozinha ~0,5–1 s depois do sinal voltar (Stop/Run + reset do Feed). O modo é o que escolheste no lobby — não está travado em 2560×1440. A retomada ainda pode dar um salto; o próximo passo é deixar isto mais suave.

### Novidades 3.1.5

- **Iniciar aplica o CFG do lobby:** escala, DLSS 5, preset e NR entram na sessão (o Feed/ReShade já não ficam travados no preview).
- **CFG reabre depois do restart no lobby:** troca de Pass Count / motor volta com o painel Configurações aberto.
- **Console no overlay é opcional:** checkbox Mostrar console; marcado, as opções aparecem (padrão PS5), só texto colorido.
- **Mais consolas:** PS5, PS4, PS3, Xbox Series X/S, Xbox One, Switch 2 e Switch.
- **Marca SPIKE** à esquerda do logo N no lobby.
- **Exibir metricas** no HUD; stalls/clusters por extenso no overlay.

### Novidades 3.1.4

- **NR off de verdade em todos os x:** Delete / Ctrl+M desliga NeuralUplift **e** o feed DLSS5 (1x, 1,5x, 2x e 3x). Antes só reiniciava com `feed=ON` e a imagem quase não mudava.

### Novidades 3.1.3

- **F12** abre o CFG do lobby (a tecla já gravava, mas o menu só abria pelo botão).
- **Delete sem RenoDX** avisa para marcar no lobby — já não finge “NR OFF Confirmado”.
- **Smoke / selftest** não apagam o `NeuralUplift` do utilizador.
- Plano BDD em [`qa/bdd/`](qa/bdd/) (Gherkin: lobby, CFG do menu, Ctrl+M, atalhos, distribuição).

### Novidades 3.1.2

- **Preset visual (Default / Natural / Cinematic)** no Ctrl+M grava em **todas** as secções do ReShade (incluindo a viva `[RenoDX.DLSS5]`) e volta para a sessão — vale em 1x, 1,5x, 2x e 3x.
- **Reinício mais curto:** o aviso caiu de 2,2 s para **1,1 s**.

### Novidades 3.1.1

- **NR on/off em todos os modos:** Delete ou Ctrl+M no 1x, 1,5x, 2x e 3x grava e reinicia de volta na sessão (sem pulso ao vivo).
- **Preset visual** também reinicia no 1x — o ciclo OFF→ON invertia a imagem e o check.
- **1,75x removido** (pedia baixar a resolução do Feed).

### Novidades 3.1

- **CFG na captura (`Ctrl+M`):** o menu abre por cima do jogo, com mouse. Frame Gen entra ao vivo. **NR on/off** e o **preset visual** gravam e reiniciam — voltam para a sessão. **Esc** fecha o menu; **Esc** de novo volta ao lobby.
- **Reinício avisado:** mudar o **Pass Count**, o **preset visual**, o **NR**, ou aplicar nova **escala do Feed** reinicia o app — o addon lê essas coisas só no boot. O aviso "Reiniciando o NexusCap" fica na tela durante a contagem, o teardown e o boot do processo novo, e a sessão volta onde estava (nunca no lobby).
- Pitch 3.1: HDMI de qualquer consola, Frame Gen até 4x se o Lossless Scaling estiver na Steam (integra sozinho, perfil de mira/HUD), stills **STABLE** em 1440p.
- Se um modo pesar: escala do Feed (onde o DLSS trabalha) ou modo da placa (o HDMI que ela anuncia). Ex.: placa até 4K30, preferência 2K60.
- Tabela de latência inclui **3x** (~33 ms, ~27 fps). **2x** ~25 ms / ~40 fps. **1,5x** continua o modo justo (~7 ms, ~60 fps).

### Novidades 3.0

- **Pass Count 1x / 1,5x / 2x / 3x:** **1x** e **1,5x** ficam nativos a ~60 fps. **2x / 3x** nativos caem de fps. Escala do Feed continua **manual**.
- **Escala DLSS5 (Feed):** lobby sempre 100% (menus nítidos); escala na captura via reinício; mínimo efetivo 50% no Feed.
- **Overlay:** cadeia captura → trabalho → saída; estado DLSS5 visível.
- **NR on/off:** Delete ou Ctrl+M em qualquer Pass Count (reinicia e volta para a sessão).

### Qualidade e latência (3.0)

- Captura **NV12-first**; rejeita downscale silencioso (ex.: 4K60 → 1080p) — sem fake/tela branca no lobby.
- Lobby lista só modos que anunciam NV12; falha de open volta ao lobby (sem fallback fake).
- Hitch / overlay **STABLE** escalam com o FPS da captura (`25×60/fps`) — sem falso instável em 30 fps.
- **Latência no HUD (qualquer modo · escala/work 100%):**
  - **`present` (+NR/DLSS5)** = processamento no Present **com enhance ligado** — no **1x** ~3,5 ms; no **1,5x** ~7 ms (ainda cabe nos ~16 ms @60).
  - **`cap`** = captura pura no app — tipicamente **~1 ms**.
  - **2x** neste setup saiu a ~40 fps / present ~25 ms; **3x** ~27 fps / present ~33 ms. Por isso o 1,5x é o modo do dia a dia.
- Mini overlay: `cap … · present …ms+NR` · HUD F3: `present … (+NR/DLSS5)`.
- Instalador **Release** sem janela preta de CMD (só GUI). **Sem admin / sem UAC** — pasta do utilizador, menos invasivo.

### Prints (HDMI de qualquer console · God of War · 1440p60 · 100% · VSYNC · FG off)

Sinal HDMI **direto de qualquer console** (PS5, Xbox Series S, Switch 2, outro HDMI), sem recapturar o PC. A escada de look e de uma **sessao PS5**. **1,5x e o modo do dia a dia.** **2x otimizado** e o look pesado que recomendamos se o PC aguentar.

Voce ve a imagem **direto**: preview no lobby, captura no Present, OBS Game Capture pega o mesmo frame (HUD/mini overlay queimados). No **1x** nativo (100%, VSYNC) o ritmo fica ~59,9 fps, stalls 0, Feed leve.

| | |
|---|---|
| Capa / lobby (TLOU Parte II) | ![capa](assets/pitch/print_cover.jpg) |
| Sem DLSS / NR | ![sem NR](assets/pitch/print_compare_nroff.jpg) |
| **1x** · 59,9 fps · stalls 0 | ![1x](assets/pitch/print_compare_1x.jpg) |
| **1,5x** · 59,9 fps · stalls 0 · recomendado | ![1,5x](assets/pitch/print_compare_15x.jpg) |
| **2x** default · ~40 fps | ![2x](assets/pitch/print_compare_2x.jpg) |
| **2x otimizado** · ~40 fps · menos flicker | ![2xopt](assets/pitch/print_compare_2xopt.jpg) |
| **3x** · ~27 fps · teto | ![3x](assets/pitch/print_compare_3x.jpg) |
| PS5 (sessao TLOU) | ![ps5](assets/pitch/print_ps5_session.jpg) |
| Switch 2 (sessao) | ![sw2](assets/pitch/print_switch2_session.jpg) |
| Xbox Series S (sessao) | ![xss](assets/pitch/print_xbox_session.jpg) |

Pitch em PDF: [`assets/pitch/NexusCap_Pitch.pdf`](assets/pitch/NexusCap_Pitch.pdf)

- 1x e 1,5x: **mesma resolução, mesmo ~60 fps**. O 1,5x fica mais limpo (pele, cenário) sem perder o ritmo.
- 2x e 3x: mais custo, pouco a ganhar, look mais descaracterizado.
- Escala do Feed é escolha **manual** — os modos 1x/1,5x não baixam a res sozinhos.
- Guardrail: mudanças de enhance não devem piorar nitidez nem `present_ms` vs 1.1.7.
- Com RenoDX ligado, escala UI → `work_resolution` do Feed apenas (sem double-downsample no app).
- **VSYNC default** (`Sincronizar imagem`) — TEAR+ALLOW_TEARING ficou mais “tremido” no A/B; Present(1) no Hz do monitor.
- NR no boot confia **NeuralUplift** (sem enable-pulse que invertia OFF).
- Métricas smoke: **120 frames de warmup** (hitch NGX ~2,5 s) fora da janela de 600; regime steady ~**59,9 fps**, `p95_present` ~2,1–2,4 ms.
- **Mini overlay**: 2 linhas · latência `cap` / `present+NR` · FT/L600/HW · 3 resoluções.
- **Preview lobby**: áudio WASAPI. **Desativar preview** / **Desativar som** na faixa. Preview off para a captura HDMI do lobby; Iniciar não muda.
- **Ctrl+Espaço** (default, configurável em Atalhos + checkbox Ctrl): cicla o HUD na sessão (Compact → Mini → Advanced → Live → off → Compact). Não grava os checks do lobby.
- **Reportar bug** → `report/bug_TIMESTAMP.txt` (eventos, hitch, `event=exception` em falhas).
- **Reportar performance** → `performance/performance_TIMESTAMP.txt` (SETUP PC, snapshot RenoDX, TUNABLES, métricas/latência, AI_HINT) — overhead mínimo (heartbeat ~2 s).

---

## Por que existe (vantagem principal)

Muitos viewers “bonitos” **capturam a tela** (ou a janela de outro app) para aplicar filtro/upscale. Isso gera:

- hitch / stutter aleatório (compositor e capturador brigam pelo mesmo frame)
- latência extra (frame → tela → recaptura → filtro → tela de novo)
- ghost / inconsistência quando Frame Gen entra no meio

**NexusCap não faz isso.** Ele lê o **HDMI da placa de captura** e desenha **ele mesmo** em D3D11. O enhance (RenoDX) e o Frame Gen entram **depois**, no Present — uma passagem limpa:

```text
  console (PS5 / Xbox / Switch 2 / ...)  --HDMI--> placa --DShow--> NexusCap (D3D11 Present)
                                              |
                                              |- RenoDX / NR  (opcional)
                                              +- Lossless FG  (opcional)
```

Resultado: imagem estável, latência previsível, sem o “tranco” clássico de recapturar a própria saída.

### Arquitetura 6.0 — a tela fica, o motor aplica

O Krish 8.5 lê Pass Count e look **só no init**. Não dá para reler o X no processo vivo. A 5.x matava a janela inteira. A 6.0 separa o que vês do que lê o INI:

```mermaid
flowchart LR
  HDMI[Console HDMI] --> Cap[NexusCap captura DShow / WGC]
  Cap --> Present[Present + ReShade 8.5]
  Present --> Tela[Janela visivel]
  X[Troca de X / NR / preset / escala] --> Hold[GDI segura o ultimo frame]
  Hold --> Novo[Processo novo le o INI]
  Novo --> Present
```

| Peça | Quem é | O que faz |
|------|--------|-----------|
| **Janela / imagem** | GDI do último frame | Não apaga. Sem segunda tela GPU. |
| **Motor** | O mesmo `NexusCap.exe` no Present | ReShade 8.5 ao lado do exe. Um GPU. |
| **Sessão** | `--autostart` | Volta à captura, não ao lobby. |
| **HDMI / FG** | Host, como na 5.x | FLIP_DISCARD, waitable, Flow 80, WGC, FG FIXED. |

O lobby não carrega `dxgi` (salvo preview CFG ao vivo). Iniciar entra direto na sessão no mesmo processo; só relança em silêncio se o ReShade já estiver no menu. Troca de X no lobby só grava — o motor entra no Iniciar.

---

## Por que C++17

| Vantagem | Na prática |
|----------|------------|
| **Latência baixa** | Sem GC, sem runtime pesado no caminho do frame |
| **Controle total de GPU/API** | D3D11, shaders NV12→RGB, Present sync — no mesmo processo |
| **DirectShow / WASAPI nativos** | Captura e áudio HDMI sem ponte lenta |
| **Determinismo** | Frametime e hitch medidos de verdade (HUD / analyze) |
| **Binário único Release** | `.exe` + DLLs ao lado; fácil de versionar e distribuir em privado |

ImGui é só UI do lobby; o caminho quente (captura → Present) é C++ + D3D11.

---

## Arquitetura (4.0)

```mermaid
flowchart TB
  subgraph IN["Entrada"]
    HDMI["Qualquer console<br/>HDMI"]
    CARD["Placa de captura<br/>DirectShow"]
    HDMI --> CARD
  end

  subgraph APP["NexusCap · C++17 · v4.0"]
    direction TB
    LOBBY["Lobby ImGui<br/>preview CPU · CFG<br/>MGF Techniques idle se RenoDX off"]
    CAP["DShowCapture<br/>NV12 scratch + FrameSlot recycle"]
    SLOT["FrameSlot<br/>latest-wins · ping-pong"]
    REND["Renderer D3D11<br/>YUV→RGB · Present nativo se RenoDX<br/>scale app so sem RenoDX"]
    HUD["Mini overlay 2 linhas<br/>ADV+HW · 3 res · lat p/e2e"]
    LOBBY -.->|Iniciar · MGF chain ON| CAP
    CARD --> CAP --> SLOT --> REND
    REND --> HUD
  end

  subgraph FX["Enhance / FG · no Present"]
    FEED["dlss5-feed<br/>work_resolution = escala UI"]
    RENO["ReShade + RenoDX NR<br/>NeuralUplift boot · Bridge Delete"]
    FG["Lossless Scaling<br/>LSFG3 mira-lock"]
  end

  REND --> FEED --> RENO
  REND --> FG
  RENO --> OUT["Monitor · VSYNC default"]
  FG --> OUT

  style IN fill:#0d1b2a,stroke:#1b9aaa,color:#e8f1f2
  style APP fill:#13293d,stroke:#3e92cc,color:#e8f1f2
  style FX fill:#1b2838,stroke:#f4a261,color:#e8f1f2
```

### Camadas

```text
┌─────────────────────────────────────────────────────────────┐
│  LOBBY          UI · preview · settings.ini · health/logs   │
├─────────────────────────────────────────────────────────────┤
│  SESSION        hotkeys · Esc→lobby · analyze · avisos      │
├─────────────────────────────────────────────────────────────┤
│  CAPTURE        DirectShow · NV12 · FrameSlot recycle       │
├─────────────────────────────────────────────────────────────┤
│  RENDER         D3D11 decode · Present DXGI (VSYNC default) │
│                 Com RenoDX: Present nativo (sem scale app)  │
├─────────────────────────────────────────────────────────────┤
│  FEED / ENHANCE escala so no Feed · RenoDX/NR · Bridge      │
├─────────────────────────────────────────────────────────────┤
│  FRAME GEN      Lossless Scaling (Steam) · perfil mira-lock │
└─────────────────────────────────────────────────────────────┘
```

**Regra de ouro:** a captura **nunca** olha para o framebuffer já filtrado. Por isso o hitch de “capturar a própria tela” não aparece aqui.

**Contrato de escala (4.0):** com RenoDX, o slider de escala **não** reduz o RT do app (evita double-downsample). Só o Feed recebe `work_resolution`. Sem RenoDX, o scale GPU do renderer continua valendo.

### Comparativo rápido

| Abordagem | Caminho | Risco de hitch |
|-----------|---------|----------------|
| Recapturar tela/janela + filtro | sinal → app A → tela → capturador → app B | alto |
| Outro viewer + Feed/NR | VEDO → app de overlay + Feed/NR | baixo (bom), menos telemetria |
| **NexusCap 4.0** | sinal → placa → **um** Present D3D11 (+ Feed/NR/FG) | baixo + métricas 600f / analyze |

---

## O que o programa faz

1. **Lobby** — dispositivo HDMI/webcam, modo (resolução/FPS), formato (NV12…), cor/range, escala, RenoDX/FG na engrenagem (CFG). Preview ao vivo (decode CPU letterbox). Com RenoDX off, Techniques MGF ficam vazias (lobby leve).
2. **Iniciar processamento** — sessão: captura → decode NV12 → Present. Com RenoDX: Present nativo + escala no Feed; sem RenoDX: scale GPU no renderer. Lossless Scaling se FG ligado.
3. **Esc** na sessão volta ao lobby (feed volta a 100%; MGF conforme RenoDX). Esc no lobby **não** fecha o app (só X / Sair).

### O que *não* é

- Não embute Lossless Scaling.
- Preview do lobby **não** passa por RenoDX/NR no ImGui (CPU → textura); o Present do lobby ainda carrega dxgi se o stack estiver ao lado do exe.
- **Streamline não entra no uso normal.** O enhance do app é RenoDX. Com RenoDX ligado o Streamline fica off (HDMI não se dá bem com DLSS nativo de jogo; os dois no mesmo Present ainda processariam duas vezes).

---

## Fluxo do frame (detalhe)

```
Placa HDMI (DShow)
    → FrameSlot (NV12 recycle / latest-wins)
        → Renderer D3D11 (YUV→RGB)
            → Present DXGI  [VSYNC default · TEAR opcional no CFG]
                → dlss5-feed (work_resolution = escala UI)   [se RenoDX]
                → ReShade/RenoDX Neural Rendering            [opcional]
                → Lossless Scaling LSFG3 (mira-lock)         [opcional]
```

- **Com RenoDX:** Present = resolução do sinal; escala só no Feed (`work_resolution`).
- **Sem RenoDX:** escala 25–100% no renderer (trabalho GPU).
- **4K @ 50%** (sem RenoDX) = captura 3840×2160, work ~1920×1080. Mínimo **25%**.
- **4K** ok se a placa listar o modo — sem teto artificial no app.

---

## Lobby e engrenagem (CFG)

| Área | Função |
|------|--------|
| **Dispositivo / Modo** | Combos DirectShow reais (é aqui que se escolhe a placa). |
| **Formato / Cor / Range / Buffer** | Estilo OBS; NV12 + BT.709 + Limitado por default. |
| **Escala** | 25–100% — com RenoDX: só Feed; sem RenoDX: trabalho GPU do app. |
| **Frame Gen** | x2/x3/x4; **Modo** Fixo ou Adaptive; **Pasta** se o Steam não achar o exe; **Auto** limpa override. |
| **RenoDX / Neural Rendering** | Checkbox + presets Default / Natural / Cinematic; Delete liga/desliga NR. |
| **Sincronizar imagem** | Default **ON** (VSYNC). Off = TEAR / Present livre. |
| **Abrir logs** | Pasta `%LOCALAPPDATA%\NexusCap\logs`. |
| **Atalhos** | Remapeáveis, incluindo o **Menu na captura** (`Ctrl+M`). |

Avisos leves (lobby toast + HUD na sessão): ao **ligar FG**, ao **subir** o multiplicador (2→3→4), e ao **ligar/desligar** Neural Rendering. Na sessão o tip aparece mesmo se o HUD estiver oculto.

---

## Enhance: RenoDX (o que usamos)

O caminho do produto é **RenoDX** — ReShade + addons DLSS 5 no Present, NR via Bridge, presets no CFG. É o que o lobby liga.

**Streamline** (DLSS nativo NVIDIA) ficou no código/instalador como backend antigo (`--enhance streamline`). Não aparece no menu. HDMI não tem vetores de movimento de jogo, então esse caminho é pior; com RenoDX ligado ele **não** roda.

| Backend | No app | Papel |
|---------|--------|--------|
| **RenoDX** | default do CFG (`enhance_renodx`) | Feed + Neural Rendering. **Usamos isto.** |
| **Off** | RenoDX desmarcado | Passthrough + FG se quiser. |
| **Streamline** | só CLI legado | Não usar no HDMI. |

Setup: `tools\setup_renodx.ps1` copia `dxgi.dll`, addons, shaders/preset. O `nvngx_dlss*.dll` da NVIDIA (RenoDX/DLSS 5, não Streamline) fica só em `addons-stable` e `addons-multipass` — o Feed lê a pasta do addon, não a raiz do exe.

---

## Frame Gen (Lossless Scaling)

- Instala pelo Steam. Busca: caminho da CFG / `--fg-path` → pasta Steam padrão → registro Steam.
- Perfil **mira-lock**: Flow 80, WGC, LSFG3 **FIXED** por default, F24. O NexusCap não embute o LS.
- Page Down = on/off · Page Up = ciclo x2→x3→x4 (só no **Fixo**).

### Fixo vs Adaptive

No lobby (engrenagem) e no CFG da sessão (`Ctrl+M`): combo **Modo FG**.

| Fixo | Adaptive |
|---|---|
| ![Fixo](assets/pitch/print_fg_fixo.png) | ![Adaptive](assets/pitch/print_fg_adaptive.png) |

| Modo | O que faz | Como aplica |
|------|-----------|-------------|
| **Fixo (x2/x3/x4)** | Sempre multiplica a captura: 40 fps ×2 = 80, ×3 = 120, ×4 = 160. | Vale na hora. Grava FIXED e reabre o LS. Radios x2/x3/x4 só neste modo. |
| **Adaptive (teto FPS)** | Gera frames extra até o que você definiu. Captura 40, alvo 75: sobe até 75. | No lobby/sessão: escreve o alvo e **Aplicar Adaptive**. Sem Aplicar, fechar não muda o default (Fixo). No **Benchmark** o Iniciar já carrega o plano, sem botão Aplicar. |

**Alvo FPS:** o valor que você definiu na saída. Digita o número (sem + e −). Piso = FPS da captura **sem** FG (HDMI a 40 não pode 30). Teto = Hz do monitor. **Hz do monitor** preenche o máximo.

O overlay (HUD, mini e live) mostra o modo ao vivo: `FG Fixo x2` ou `FG Adaptive 75`.

Fixo é um multiplicador fechado. Adaptive gera frames até atingir o alvo. Default continua Fixo. Adaptive é opcional.

---

## Mapa do código

| Caminho | Papel |
|---------|--------|
| `src/main.cpp` | Sessão, hotkeys, FG/NR, analyze, volta ao lobby |
| `src/lobby_imgui.cpp` | Lobby, preview, CFG, logo, browse FG |
| `src/capture_dshow.cpp` | DirectShow, modos/fourcc |
| `src/capture_fake.cpp` | Smoke/selftest |
| `src/renderer_d3d11.cpp` | Decode, scale, Present, HUD, ícone |
| `src/lossless_fg.cpp` | Resolve LS, mira-lock, Job Object |
| `src/dlss5_bridge.cpp` | Pulse Neural Rendering |
| `src/enhance_dlss_streamline.cpp` | Streamline DLSS (legado, off com RenoDX) |
| `src/enhance_passthrough.cpp` | Sem enhance |
| `src/settings_store.cpp` | `settings.ini` + sync feed/ReShade |
| `src/config.cpp` | CLI + persist → config |
| `src/audio_wasapi.cpp` | Áudio (prefere HDMI) |
| `src/analyze_io.cpp` | JSONL + summary |
| `include/nexuscap/*.hpp` | Contratos e helpers testáveis |
| `tests/test_core.cpp` | Unit (doctest) |
| `resources/nexuscap.rc` | Ícone no `.exe` |
| `assets/logo.png` | Brand |
| `third_party/` | imgui, doctest, Streamline SDK (vendored, não é o enhance do CFG) |
| `tools/setup_renodx.ps1` | Stack RenoDX ao lado do Release |

---

## Build

**Requisitos:** VS 2022 (C++), CMake ≥ 3.20, Windows SDK. NVIDIA recomendada para RenoDX / Neural Rendering.

```bat
cd C:\Users\Leo\Documents\NexusCap
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
powershell -ExecutionPolicy Bypass -File tools\setup_renodx.ps1
```

Ou `build.bat`. POST_BUILD copia DLLs NVIDIA (`nvngx` / Streamline legado) + `assets/logo.png` e `assets/watermark.png`. Ícone do exe vem do `.rc`.

---

## Instalador (Inno Setup)

Gera `dist\NexusCap-Setup-<versão>.exe` e `dist\NexusCap-Portable-<versão>.zip`. **Não inclui Lossless Scaling** (Frame Gen continua opcional via Steam).

**Requisitos extras:** [Inno Setup 6+](https://jrsoftware.org/isinfo.php) (o script pode instalar via winget) e, para embutir RenoDX, a pasta X2 (`C:\Program Files\DLSS5 MGF X2`).

```bat
tools\make_installer.bat
rem ou:
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -Version 4.1.8
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -InstallInno
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 -SkipBuild -Version 4.1.8
```

Instala por padrão em `%LOCALAPPDATA%\NexusCap` (`PrivilegesRequired=lowest` no Inno — **sem UAC / sem admin**). É menos invasivo de propósito: o setup não mexe no sistema. **Evite Program Files** — RenoDX precisa gravar ini/cfg/cache ao lado do exe.

| Peça | Caminho |
|------|---------|
| Script Inno | `packaging/NexusCap.iss` |
| Stage limpo | `dist/stage/` |
| Aviso LS | `packaging/NOTICE.txt` (wizard + pasta do app) |
| Saída | `dist/NexusCap-Setup-*.exe` e `dist/NexusCap-Portable-*.zip` |

Desinstalação remove o app; **não** apaga `%LOCALAPPDATA%\NexusCap\settings.ini`. O zip portable não tem desinstalador — apague a pasta.

O `portable.txt` **não** entra no setup. Só no zip. Copiar a pasta instalada sem esse arquivo continua gravando no AppData deste Windows.

---

## Uso

```bat
build\Release\NexusCap.exe
build\Release\NexusCap.exe --renodx --no-lobby
build\Release\NexusCap.exe --renodx --fg-x 2 --no-lobby
build\Release\NexusCap.exe --scale 50 --no-lobby
build\Release\NexusCap.exe --smoke --frames 120
build\Release\NexusCap.exe --selftest-fg --fake-capture
```

### CLI

| Flag | Efeito |
|------|--------|
| `--no-lobby` | Direto à sessão |
| `--renodx` / `--enhance renodx\|off` | Backend do produto (RenoDX) |
| `--enhance streamline` / `--dlss` / `--dlss-mode …` | Legado Streamline — não usar no HDMI |
| `--fg-x 1\|2\|3\|4` | FG (1 = off) |
| `--fg-path <exe>` | Override Lossless Scaling |
| `--scale 25–100` | Trabalho GPU |
| `--device <nome>` | Filtro dispositivo |
| `--windowed` | Janela |
| `--portable` | Settings/presets/logs na pasta do exe (o zip já traz `portable.txt`) |
| `--smoke` / `--frames N` / `--fake-capture` | Testes |
| `--selftest-cap` / `--selftest-dlss` / `--selftest-fg` / `--selftest-cfg` | Selftests |

### Hotkeys padrão (CFG remapeia)

| Tecla | Ação |
|-------|------|
| Esc | CFG aberto → fecha o menu; sem menu, sessão → lobby |
| Ctrl+M | CFG na captura (por cima do jogo) |
| F3 | HUD |
| F4 | Analyze |
| F5 | Streamline DLSS (legado; com RenoDX não faz nada) |
| Page Down | FG on/off |
| Page Up | Ciclo FG |
| Home | Overlay RenoDX |
| Delete | Neural Rendering on/off |
| F11 | Fullscreen ↔ janela |
| F12 | CFG no lobby |

---

## Persistência e logs

- Settings: `%LOCALAPPDATA%\NexusCap\settings.ini` (instalado) ou `settings.ini` ao lado do exe (portable)
- FG mira-lock: `%LOCALAPPDATA%\Lossless Scaling\Settings.xml` (sempre desta máquina — não viaja no zip)
- Logs: `logs\` em AppData (instalado) ou ao lado do exe (portable)

---

## Testes

```bat
ctest --test-dir build -C Release --output-on-failure
build\Release\nexuscap_tests.exe
```

`ctest` corre unitários + smokes + `--selftest-fg` / `--selftest-cfg`. Smoke e selftest **não** apagam o NeuralUplift do utilizador.

BDD (Gherkin, HDMI à mão): [`qa/bdd/`](qa/bdd/) — ordem no `README` dessa pasta. `@critico` é porta de release. `@auto` = já coberto pelo `ctest`.

Unitários cobrem CLI, escala, mira-lock, Esc/WM_QUIT no lobby, F12 do CFG, Bridge NR, formato/cor, letterbox, avisos FG/NR, `settings.ini`.

---

## Terceiros

imgui · doctest · NVIDIA nvngx (RenoDX/DLSS 5) · RenoDX/ReShade/feed (via setup) · Lossless Scaling (Steam do usuário). Streamline SDK fica vendored/legado e não entra no fluxo do CFG.
