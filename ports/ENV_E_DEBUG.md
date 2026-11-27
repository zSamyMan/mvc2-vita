# env.txt e ferramentas de debug (PS Vita)

O arquivo `ux0:data/mvc_2/env.txt` tem uma variável por linha, no formato `NOME=valor`. Ele é lido uma vez, quando o
jogo inicia. O menu de configurações (abaixo) grava as opções do jogador nele; as outras linhas são mantidas. Uma variável ausente usa o padrão indicado. Só as variáveis abaixo são aceitas: qualquer outra linha é
ignorada (e anotada no `mvc2.log` como "left out"). Os ajustes internos do port usam sempre os seus padrões.

## Opções do jogador

| Variável | Padrão | Descrição |
|---|---|---|
| `ASPECT_RATIO` | `WIDE` | `ORIGINAL`: 4:3 com as bordas ilustradas nas laterais. `STRETCHED`: 4:3 esticado para 16:9. `WIDE`: widescreen real, com a interface ajustada aos cantos. |
| `PRESET` | `BALANCED` | Os clocks. `STOCK`: os do próprio Vita (ARM 333, GPU 166, crossbar 111 MHz, barramento 166); os Hyper Combo Finish mais pesados podem perder quadros. `BALANCED`: ARM 333, GPU 222, crossbar 111, barramento 166 (60 FPS constantes no Abyss). `PERFORMANCE`: ARM 444, barramento e GPU 222, crossbar 166; esquenta mais e gasta mais bateria. |
| `BORDERS` | `ARTWORK1` | Só com `ASPECT_RATIO=ORIGINAL`: `ARTWORK1` e `ARTWORK2` mostram um dos dois artworks nas laterais (`ARTWORK`, o valor antigo, vale como `ARTWORK1`); `NONE` deixa as laterais pretas. |
| `CONTROL_SCHEME` | `VITA` | Os controles (as imagens da tela CONTROL TYPE). Em todos: quadrado soco fraco, triângulo soco forte, X chute fraco, círculo chute forte. `VITA`: L assist 1, R assist 2. `PS2`: L assist 1, painel traseiro esquerdo assist 2, R troca para o parceiro 1, painel traseiro direito troca para o parceiro 2. `GRIP`: L assist 1, R assist 2, painel traseiro direito soco fraco + forte, esquerdo chute fraco + forte. O painel traseiro conta só na metade de cima. |
| `RIGHT_STICK` | `ON` | O analógico direito do esquema. `VITA` e `PS2`: cima soco fraco + forte, baixo chute fraco + forte. `GRIP`: cima troca para o parceiro 1, baixo para o parceiro 2. `OFF`: o analógico direito não faz nada. |
| `SECRET_FACTOR` | (o do jogo) | `UNLOCKED`: todos os personagens, cores e extras secretos liberados. `LOCKED`: liberados jogando, como no Dreamcast. |
| `CONFIG_MENU` | `ON` | `ON`: o menu de configurações aparece a cada início. `OFF` ("Don't show this again at startup"): só aparece segurando SELECT enquanto o jogo inicia. |
| `DEBUG_OPTIONS` | `1` | `1` liga as ferramentas de teste: log (`mvc2.log`), menu de debug, overlay, atalhos com SELECT (capturas, dumps, sequências), a imagem do renderizador por software junto de cada captura e as variáveis de teste abaixo. `0` desliga tudo isso: é a configuração de quem só vai jogar. |

## Menu de configurações

Aparece depois da extração do disco, antes do jogo (com `CONFIG_MENU=OFF`, só segurando SELECT no início). Duas
telas, com o fundo e as pílulas do menu de opções do Dreamcast (`DM0CTEX.BIN`) e a fonte do jogo
(`S24RM04.BIN`):

- **GAME SETUP**: ASPECT RATIO, SECRET FACTOR, RIGHT ANALOG STICK, PRESET e DEBUG OPTIONS. UP/DOWN escolhem,
  LEFT/RIGHT (ou X) mudam; NEXT (X) vai para a próxima tela. A caixa de baixo explica a opção escolhida. Com
  ORIGINAL, uma caixa ao lado de ASPECT RATIO escolhe NO BORDERS, ARTWORK 1 ou ARTWORK 2 (DOWN a partir de ASPECT RATIO).
- **CONTROL TYPE**: VITA, PS2 ou GRIP, sobre a imagem do esquema; "Don't show this again at startup" (X marca);
  BACK (ou O) volta; START GAME começa o jogo.

Também funciona pelo toque na tela: tocar num item o seleciona; tocar de novo nele muda o valor (nas setas, para
aquele lado). NEXT, BACK, START GAME, a caixa de seleção e "Press START to save" agem no primeiro toque.

START, em qualquer tela, grava o `env.txt` e começa o jogo. ASPECT RATIO, BORDERS, PRESET, os controles e SECRET FACTOR valem
na hora; DEBUG OPTIONS vale a partir do próximo início. Os scripts de teste gravam `CONFIG_MENU=OFF`.

## Toque nos menus do jogo

Fora da luta (e com o jogo pausado), a tela de toque navega os menus do próprio jogo: arrastar o dedo mais de 40
pixels é o direcional para aquele lado (segurando, repete); um toque é X (confirma); dois dedos são O (volta);
segurar parado por cerca de 2/3 de segundo é START. Durante a luta o toque não faz nada.

## Variáveis de teste (só com `DEBUG_OPTIONS=1`)

Usadas pelos scripts de teste automáticos; ignoradas com `DEBUG_OPTIONS=0`.

| Variável | Descrição |
|---|---|
| `MVC2_STAGE` | `0`–`16`: o estágio da primeira luta (e o valor inicial da linha STAGE do menu). |
| `MVC2_BOSS` | `1`–`7`: o Arcade começa naquela luta (`7` é o Abyss). |
| `MVC2_TEST` | Bits, que podem ser somados. `1`: CPU com 1 de vida. `2`: barra de hyper do humano cheia. `4`: anota a vida da CPU no log a cada segundo. `8`: vida do humano sempre cheia. `16`: sequência de capturas a cada Hyper Combo Finish. Os bits 1, 2 e 8 também podem ser mudados pelo menu. |

## Atalhos no jogo (com `DEBUG_OPTIONS=1`)

| Atalho | Ação |
|---|---|
| **L + DOWN, depois SELECT** | Abre e fecha o **menu de debug** (detalhes abaixo). Aperte L e DOWN primeiro: se o SELECT entrar junto com só um deles, dispara a captura (SELECT+L) ou a sequência de capturas (SELECT+DOWN). |
| **SELECT + L** | Captura: `shots/shot_N.bmp` (o que a GPU desenhou) e `soft_N.bmp`, o mesmo frame no renderizador por software. |
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
| STAGE < n >  X: APPLY | LEFT/RIGHT escolhem o estágio (0 a 16, ou o do jogo); X aplica: sai da luta para a tela de seleção de personagens (pelo mesmo caminho do CHARACTER CHANGE do Training) e a próxima luta usa o estágio escolhido. Vale no Arcade, no Versus e no Training; só sai para a seleção durante um jogo. |
| NEXT FIGHT: ABYSS | A próxima luta do Arcade é contra o chefe. |
| GO TO: STAFF ROLL | Vai para os créditos (só durante um jogo). |
| GO TO: END LOGO (CAPCOM) | Vai para a logo final: um segundo de tela preta, depois a logo com o flash, e "THANK YOU FOR PLAYING" fica na tela até START. Uma sequência de capturas é tirada sozinha no momento do flash. |
| GO TO: ATTRACT / TITLE | Volta para o modo de demonstração e o título. |
| PERFORMANCE OVERLAY | Liga e desliga o overlay (FPS, frames perdidos, tempos do jogo, do som e do render, triângulos e CPU por thread). Começa desligado. |
| CLOSE | Fecha o menu. |

A segunda linha do menu mostra onde o jogo está: modo, cena, estágio atual e número da luta.

## Primeira execução

Coloque a imagem do disco (USA: `.chd`, `.gdi` com as trilhas, `.cue` com os `.bin`, `.iso` ou só a trilha 3) em
`ux0:data/mvc_2` ou em `ux0:data/mvc_2/disc`. Na primeira vez que o port abre, os arquivos do disco são extraídos dela
para `ux0:data/mvc_2/disc` (uma imagem em `ux0:data/mvc_2` é antes movida para lá, sem cópia). Durante a extração aparece
a tela de primeira execução, com a barra de progresso enchendo em amarelo. Terminada e conferida a extração, os
arquivos da imagem são apagados: ficam só os arquivos extraídos (cerca de 210 MB). Nas próximas execuções a tela não
aparece.

Sem imagem nem arquivos extraídos, o port mostra a tela de aviso (onde colocar o dump) até ser fechado pelo botão PS.

Para extrair no computador em vez do console, use `mvc2_extract` (`ports/tools/extract`) e copie a pasta gerada para
`ux0:data/mvc_2/disc`.

## Scripts de teste no PC (`vita-tools`, via vita-agent)

| Script | O que faz |
|---|---|
| `stage_suite.sh RUN [STAGES...]` | Percorre os cenários (0 a 16 por padrão) num só lançamento: a linha STAGE do menu de debug escolhe o cenário e volta à seleção; na luta o jogador anda para a esquerda (a câmera para antes da parede, presa ao boneco parado) e depois até a ponta direita, com uma captura em cada. `SIDE=left` pula por cima do boneco (captura com a câmera à direita do meio) e o empurra até a parede esquerda (captura). `MODE=arcade` usa o Arcade (o computador luta, vida infinita ligada) em vez do TRAINING (padrão, o boneco fica parado e a câmera chega às pontas). `DUMP=1` faz também um dump em cada ponta (os dumps travam o cartão e podem dessincronizar o menu). `ASPECT=ORIGINAL`/`STRETCHED` troca o aspecto (padrão WIDE). |
| `ab_stage.sh RUN STAGE [LINHAS...]` | Luta de TRAINING no cenário dado (`MVC2_STAGE`), jogador levado à ponta direita e deixado lá 40 s, sem capturas; imprime fps, vblanks perdidos, prep, GL, swap e o tempo da correção de ordem translúcida dos últimos 36 s. `WALKS=n` troca as 14 andadas (`WALKS=0`: fica na posição inicial). Não instala o build: rode `deploy.sh 5` antes. As linhas extras vão para o env.txt (só as que ele aceita, veja acima). `PRE="passos"` insere passos antes da espera (ex. `PRE=dbgmenu`). |
| `user_shots.sh OUTDIR [DATA]` | Baixa as capturas PS+START (`ux0:picture/SCREENSHOT`) feitas na data dada. |
