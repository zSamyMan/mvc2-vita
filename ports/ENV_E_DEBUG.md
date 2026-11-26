# env.txt e ferramentas de debug (PS Vita)

O arquivo `ux0:data/mvc_2/env.txt` tem uma variável por linha, no formato `NOME=valor`. Ele é lido uma vez, quando o
jogo inicia. Uma variável ausente usa o padrão indicado. Os valores `1`/`0` ligam e desligam.

As variáveis marcadas com **(debug)** só funcionam com `DEBUG_OPTIONS=1`. Com `DEBUG_OPTIONS=0`, que é a configuração
de quem só vai jogar, elas são ignoradas e todas as ferramentas de teste ficam desligadas.

## Opções do jogador

| Variável | Padrão | Descrição |
|---|---|---|
| `ASPECT_RATIO` | `WIDE` | `ORIGINAL`: 4:3 com barras laterais. `STRETCHED`: 4:3 esticado para 16:9. `WIDE`: widescreen real (experimental), com a interface ajustada aos cantos. |
| `CLOCKS` | `333` | `333`: perfil padrão do Vita (ARM 333, GPU 166, crossbar 111 MHz; barramento 222). `444`: ARM 444, barramento e GPU 222, crossbar 166. Esquenta mais e gasta mais bateria; serve para comparar. |
| `DEBUG_OPTIONS` | `1` | `0` desliga todas as ferramentas de teste: log (`mvc2.log`), overlay, capturas, dumps, trace, perfis, menu de debug e os atalhos com SELECT. Use `0` numa build para jogadores. |
| `MVC2_SOUND` | `1` | `0`: sem som. |
| `MVC2_UNLOCK` | — | Libera os personagens e extras secretos (para testes). |
| `MVC2_FREEPLAY` | — | Partidas sem crédito (padrão da versão de arcade). |

## Atalhos no jogo (com `DEBUG_OPTIONS=1`)

| Atalho | Ação |
|---|---|
| **L + DOWN, depois SELECT** | Abre e fecha o **menu de debug** (detalhes abaixo). Aperte L e DOWN primeiro: se o SELECT entrar junto com só um deles, dispara a captura (SELECT+L) ou a sequência de capturas (SELECT+DOWN). |
| **SELECT + L** | Captura: `shots/shot_N.bmp` (o que a GPU desenhou). Com `MVC2_SHOT_SOFT=1`, também `soft_N.bmp`, o mesmo frame no renderizador por software. |
| **SELECT + R** | Dump: os lotes de desenho do próximo frame em `dump_N.txt` (posições, estados, texturas), mais as capturas. Trava o jogo por alguns segundos no cartão. |
| **SELECT + DOWN** | Sequência de capturas: 30 imagens em meia resolução, uma a cada 4 frames (2 s de movimento), em `shots/burst_N_kk.bmp`. |
| **SELECT + UP** | Liga e desliga o overlay de desempenho. |
| **PS + START** | Captura do próprio sistema (`ux0:picture/SCREENSHOT/...`). É a que conta como referência do usuário nos testes. |

## Menu de debug

Com ele aberto, o jogo continua rodando, mas não recebe os botões. O menu sempre abre na primeira linha. UP/DOWN
escolhem a linha, LEFT/RIGHT mudam um ajuste, X executa e O fecha.

| Linha | O que faz |
|---|---|
| INFINITE LIFE (HUMAN) | A vida do lado humano volta ao máximo a cada frame. |
| CPU AT 1 VITAL | Os personagens do computador ficam com 1 de vida: qualquer golpe derruba. |
| HYPER GAUGE FULL | A barra de hyper do lado humano fica sempre cheia. |
| NEXT FIGHT'S STAGE < n > | Estágio da próxima luta: 0 a 16, ou o escolhido pelo jogo. Vale no Arcade, no Versus e no Training. |
| BACK TO THE SELECT (APPLY STAGE) | Sai da luta para a tela de seleção pelo mesmo caminho do CHARACTER CHANGE do Training: obras, sons, música e carregamentos são parados antes. A próxima luta usa o estágio escolhido. Só funciona durante um jogo. |
| NEXT FIGHT: ABYSS | A próxima luta do Arcade é contra o chefe. |
| GO TO: STAFF ROLL | Vai para os créditos (só durante um jogo). |
| GO TO: END LOGO (CAPCOM) | Vai para a logo final: um segundo de tela preta, depois a logo com o flash, e "THANK YOU FOR PLAYING" fica na tela até START. Uma sequência de capturas é tirada sozinha no momento do flash. |
| GO TO: ATTRACT / TITLE | Volta para o modo de demonstração e o título. |
| PERFORMANCE OVERLAY | Liga e desliga o overlay. |
| CLOSE | Fecha o menu. |

A segunda linha do menu mostra onde o jogo está: modo, cena, estágio atual e número da luta.

## Ferramentas de teste (debug)

| Variável | Descrição |
|---|---|
| `MVC2_OVERLAY` | `1`: overlay com FPS, frames perdidos, tempos do jogo, do som e do render, triângulos e uso de CPU por thread. Fica na barra esquerda, ou abaixo das barras de vida em WIDE. |
| `MVC2_SHOT` | Lista de frames (`n,m,...`) para capturar automaticamente (`shots/shot_N.bmp`). |
| `MVC2_SHOT_SOFT` | `1`: cada captura vem acompanhada da mesma imagem feita pelo renderizador por software (`soft_N.bmp`), para comparar pixel a pixel. |
| `MVC2_DUMP` | Lista de frames para fazer o dump dos lotes de desenho (`dump_N.txt`). |
| `MVC2_BURST` | Lista de frames para iniciar uma sequência de capturas. |
| `MVC2_TRACE` | `N`: os tempos dos primeiros N frames, um a um, gravados em `trace.csv` quando os N terminam. Colunas: espera, preparo, GL, swap, vblanks perdidos, uploads de textura, triângulos, tempo do jogo e do som. |
| `MVC2_TRACE_AT` | Frame em que o `trace.csv` é gravado, se for depois de N. |
| `MVC2_TRACE_GD` | Registra no log cada leitura de disco (arquivo, tamanho, tempo). |
| `MVC2_GPUTIME` | `1`: espera a GPU terminar cada frame e registra o tempo dela. Custa desempenho; serve só para medir. |
| `MVC2_CONTROL` | `1`: lê `control.txt` uma vez por segundo (`<n> shot`, `dump`, `perf` ou `gputime`). Desligado por padrão porque abrir arquivos no cartão é lento. |
| `MVC2_TTY` | `1`: o log também vai para a TTY (psp2shell, vita-agent). |
| `MVC2_SNDPROF` | `1`: divisão do tempo do som (ARM7 emulado, mixagem, resto) no log. |
| `MVC2_SNDDBG` | Contadores do áudio (amostras perdidas, filas vazias) no log ao sair. |
| `MVC2_SNDLOG` | Cada pedido de som no log. |
| `MVC2_FSX` | Só para medir, a imagem fica errada: `1` sem clamp de UV, `2` sem cor de offset, `4` sombreamento simplificado. |
| `MVC2_OBJLOG` | `n`: os modelos desenhados no frame n e os cabeçalhos dos grupos de polígonos, no log. |
| `MVC2_HUDLOG` | `n`: como a interface de luta foi deslocada para os cantos no frame n (WIDE). |
| `MVC2_EDGELOG` | Contadores dos vértices empurrados para as bordas largas (WIDE). |
| `MVC2_NOWIDESHIFT` | `1`: desliga o deslocamento lateral dos elementos de interface em WIDE (para comparar). |
| `MVC2_BOSS` | `1`–`7`: o Arcade começa naquela luta (`7` é o Abyss). |
| `MVC2_STAGE` | `0`–`16`: o estágio da primeira luta (e o valor inicial da linha de estágio do menu). |
| `MVC2_TEST` | Bits, que podem ser somados. `1`: CPU com 1 de vida. `2`: barra de hyper do humano cheia. `4`: anota a vida da CPU no log a cada segundo. `8`: vida do humano sempre cheia. `16`: sequência de capturas a cada Hyper Combo Finish. Os bits 1, 2 e 8 também podem ser mudados pelo menu. |

## Desempenho e renderização

Estas variáveis existem para medir e comparar. Os padrões são os recomendados.

| Variável | Padrão | Descrição |
|---|---|---|
| `MVC2_CUT_MOST` | `500` | Limite de candidatos de recorte de transparências por metade da lista, aplicado só em sequências pesadas: arma após 2 frames seguidos com preparo acima de 12 ms ou preparo + desenho acima de 15 ms; dura 60 frames e só é renovado enquanto preparo + desenho passar de 14 ms. Mais alto deixa a ordem mais exata e o frame mais caro. |
| `MVC2_TEX_FRAME_US` | `3000` | Orçamento de upload de texturas por frame (µs). Passando dele, um tile que já está na GPU, mudou nos 2 últimos frames e não atrasou no frame anterior é enviado no frame seguinte (os demais são sempre enviados, para não mostrar o sprite antigo do slot). `0`: sem limite. |
| `MVC2_CUTGRID` | `1` | Grade de tela para achar os candidatos de recorte. `0`: lista inteira. |
| `MVC2_CUTMT` | `1` | `0`: recortes só na thread de render, sem a segunda thread. |
| `MVC2_TRANS_AWAY` | `1` | Todos os passos da lista translúcida (pré, recortes, ordenação, correção de ordem) na thread de recorte enquanto a de render monta a lista opaca: cerca de 3 ms a menos de preparo nos cenários com muita transparência. `0`: só o pré lá, o resto depois na thread de render (comportamento anterior, para A/B). Sem efeito com `MVC2_CUTMT=0`. |
| `MVC2_HALF` | `1` | Cores em half float (muito mais rápido na GPU). `0`: float. |
| `MVC2_HWPAL` | `0` | `1`: paletas resolvidas pela própria unidade de textura (experimental). |
| `MVC2_SNAP` | `1` | Sprites amostrados no texel mais próximo, um por pixel do jogo, sem costuras. `0`: desligado. `2`: variante. |
| `MVC2_SNAP_OX` | `0` | Ajuste da origem do snap (teste). |
| `MVC2_SEAMS` | `1` | Correção de costuras entre tiles. `0`: desligada. |
| `MVC2_DEPTHMODE` | `0` | Mapeamento da profundidade (0, 1 ou 2; teste). |
| `MVC2_SHCACHE` | `1` | Pacote de shaders compilados no cartão. `0`: compila tudo na hora. |
| `MVC2_SHCTEST` | — | Teste do pacote de shaders. |
| `MVC2_TEXRING` | `1` | Rodízio de texturas da GPU para tiles que mudam a cada frame, sem alocar e liberar memória. `0`: desligado. |
| `MVC2_TEX_SKIP` | `1` | Uma textura recarregada da mesma origem no mesmo frame não é lida de novo. |
| `MVC2_TEX_KEEP_FREED` | — | `1`: texturas pequenas liberadas pelo jogo ficam guardadas para quando voltarem. |
| `MVC2_LZ_CACHE` | — | `1`: tiles já descomprimidos são copiados de volta, em vez de descomprimidos de novo. |
| `MVC2_TRIGMEMO` | `1` | Guarda senos e cossenos já calculados. `0`: não guarda. |
| `MVC2_MESH_CHECK` | — | `n`: verifica uma malha guardada a cada n desenhos (`0`: nunca). |
| `MVC2_VBO` | `1` | Vértices em buffer da GPU. `0`: na memória do app. |
| `MVC2_HUD_DZ` / `MVC2_HUD_DZ2` | `24` / `64` | Largura da faixa central que não é deslocada na interface de luta (WIDE). |
| `MVC2_ASPECT` | — | Nome antigo do formato de tela (use `ASPECT_RATIO`). |
| `MVC2_RENDERER` | `gpu` | `soft`: renderizador por software, 320×240 (muito lento; só para comparar). |
| `MVC2_RDQUALITY` | `exact` | Qualidade do renderizador por software (`fast`, `half`). |
| `MVC2_RDLOG`, `MVC2_RDPIX`, `MVC2_RDTEX`, `MVC2_RDVTX`, `MVC2_RDSAVE`, `MVC2_RDTIME` | — | Diagnósticos do renderizador por software (log, pixel, textura, vértices, salvar frames, tempos). |
| `MVC2_FPS` | `0` | O ritmo vem do vblank; outro valor força um ritmo próprio (teste). |

## Disco e cache

| Variável | Padrão | Descrição |
|---|---|---|
| `MVC2_GD_CACHE` | `90` | MB dos arquivos do disco carregados na memória no início. `0`: nenhum, tudo lido do cartão. |
| `MVC2_GD_LATE` | `6` | MB do bloco que lê antes da luta os arquivos que ficaram fora do cache (estágio e lutadores escolhidos). `0`: desligado. |
| `MVC2_GD_LAZY` | `1` | Um arquivo só é aberto no cartão na primeira leitura. |
| `MVC2_GD_NOBUF` | `1` | Leituras sem o buffer do stdio, mais rápidas. |
| `MVC2_GD_ASYNC`, `MVC2_GD_AHEAD`, `MVC2_GD_NEXT`, `MVC2_GD_KEEP`, `MVC2_GD_PROGS`, `MVC2_GD_SEEK` | — | Variantes antigas de leitura do disco (assíncrona, leitura antecipada, arquivos mantidos abertos, tempo de busca simulado). Ficam para comparar. |
| `MVC2_ADX_AHEAD`, `MVC2_ADX_FILL`, `MVC2_ADX_THREAD` | — | Variantes da leitura e mixagem da música (ADX). |
| `MVC2_VMU` | — | Cartão de memória (VMU) presente mesmo num teste com `MVC2_FRAMES`. |

## Som (avançado)

`MVC2_ARMJIT` (padrão `1`), `MVC2_ARMJIT2`, `MVC2_ARMFAST`, `MVC2_ARMSMALL`, `MVC2_ARMDEC8`, `MVC2_ARMTRACK` (`1`),
`MVC2_ARMRATE`, `MVC2_FIQFAST` (`1`), `MVC2_MIXFAST` (`1`), `MVC2_MIXSIMD`, `MVC2_TIMERFAST` (`1`), `MVC2_DSP`,
`MVC2_DSPDUMP`, `MVC2_REVERB`, `MVC2_FILTER`, `MVC2_SNDCHUNK`, `MVC2_SNDHALF`, `MVC2_SNDNATIVE`, `MVC2_SNDREQ_WAIT`,
`MVC2_SNDTRACE`, `MVC2_SNDWATCH`, `MVC2_WATCHFAST`, `MVC2_WATCHMEMO`: ajustes da emulação do chip de som (AICA e o
ARM7 dele, compilado dinamicamente), da mixagem e do DSP. Os padrões são os medidos como mais rápidos e corretos. Só
mude para comparar.

## Execuções de teste (sem console)

`MVC2_FRAMES` (número de frames a rodar), `MVC2_PRESS` (`n:tecla,...`: botões apertados a partir do frame n),
`MVC2_CLOCK` (relógio fixo), `MVC2_WAV` (som gravado num .wav), `MVC2_DC_DISC`, `MVC2_ROM` (caminhos do disco ou da
ROM): usadas pelas execuções automáticas no PC.

## Scripts de teste no PC (`vita-tools`, via vita-agent)

| Script | O que faz |
|---|---|
| `stage_suite.sh RUN [STAGES...]` | Percorre os cenários (0 a 16 por padrão) num só lançamento: o menu de debug escolhe o cenário e volta à seleção; na luta o jogador anda para a esquerda (a câmera para antes da parede, presa ao boneco parado) e depois até a ponta direita, com uma captura em cada. `SIDE=left` pula por cima do boneco (captura com a câmera à direita do meio) e o empurra até a parede esquerda (captura). `MODE=arcade` usa o Arcade (o computador luta, vida infinita ligada) em vez do TRAINING (padrão, o boneco fica parado e a câmera chega às pontas). `DUMP=1` faz também um dump em cada ponta (os dumps travam o cartão e podem dessincronizar o menu). `ASPECT=ORIGINAL`/`STRETCHED` troca o aspecto (padrão WIDE). |
| `ab_stage.sh RUN STAGE [LINHAS...]` | Luta de TRAINING no cenário dado (`MVC2_STAGE`), jogador levado à ponta direita e deixado lá 40 s, sem capturas; imprime fps, vblanks perdidos, prep, GL, swap e o tempo da correção de ordem translúcida dos últimos 36 s. `WALKS=n` troca as 14 andadas (`WALKS=0`: fica na posição inicial). Não instala o build: rode `deploy.sh 5` antes. As linhas extras vão para o env.txt (um A/B de uma opção, ex. `MVC2_RDQUALITY=nofog`). `PRE="passos"` insere passos antes da espera (ex. `PRE=dbgmenu`). |
| `user_shots.sh OUTDIR [DATA]` | Baixa as capturas PS+START (`ux0:picture/SCREENSHOT`) feitas na data dada. |

---

As variáveis de `ports/port/ctr` (3DS) e dos renderizadores de PC não valem no Vita e não estão listadas.
