# env.txt e ferramentas de debug (PS Vita)

O arquivo `ux0:data/mvc_2/env.txt` tem uma variável por linha, no formato `NOME=valor`. Ele é lido uma vez, quando o
jogo inicia. Uma variável ausente usa o padrão indicado. Só as variáveis abaixo são aceitas: qualquer outra linha é
ignorada (e anotada no `mvc2.log` como "left out"). Os ajustes internos do port usam sempre os seus padrões.

## Opções do jogador

| Variável | Padrão | Descrição |
|---|---|---|
| `ASPECT_RATIO` | `WIDE` | `ORIGINAL`: 4:3 com as bordas ilustradas nas laterais. `STRETCHED`: 4:3 esticado para 16:9. `WIDE`: widescreen real, com a interface ajustada aos cantos. |
| `CLOCKS` | `333` | `333`: ARM 333, GPU 222, crossbar 111 MHz, barramento 166 (a GPU acima dos 166 do perfil padrão do Vita, para os Hyper Combo Finish com muitas camadas transparentes). `444`: ARM 444, barramento e GPU 222, crossbar 166. Esquenta mais e gasta mais bateria; serve para comparar. |
| `DEBUG_OPTIONS` | `1` | `1` liga as ferramentas de teste: log (`mvc2.log`), menu de debug, overlay, atalhos com SELECT (capturas, dumps, sequências), a imagem do renderizador por software junto de cada captura e as variáveis de teste abaixo. `0` desliga tudo isso: é a configuração de quem só vai jogar. |

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
