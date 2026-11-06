#pragma once
// Help tab text in each language. No launcher types so the host tests can check it.
#include <array>
#include <span>
#include <string_view>

namespace help {

enum class Lang { English, Spanish, Portuguese };

struct Topic {
  const char *title;
  const char *text;  // the launcher caps a description at 1024 bytes
};

// "es_MX", "pt-BR", "es" -> by the first two letters; anything else is English.
constexpr Lang langForLocale(std::string_view locale) {
  if (locale.starts_with("es")) return Lang::Spanish;
  if (locale.starts_with("pt")) return Lang::Portuguese;
  return Lang::English;
}

inline constexpr std::array<Topic, 7> kEnglish{{
    {"Getting started",
     "1. On the BE-Matica tab, tap Import structure and pick a .mcstructure (or a Java .litematic, which is "
     "converted).\n2. Choose it under Structure. The hologram appears with its corner where you stand.\n"
     "3. Build inside the blue ghost blocks. Blocks you place correctly disappear from the hologram."},
    {"Control panel",
     "The floating panel is on screen while BE-Matica Controls is on. Drag its header (the dots) to move it; "
     "the arrow folds it away.\nThe header shows how much of the shown structure is built.\n"
     "Move tab: the D-pad shifts the hologram one block the way you're facing (up = forward, left = your left), the ▲ / ▼ "
     "pair moves it up and down, Rotate +90° turns it, Mirror flips it, "
     "< and > pick a single layer, All shows every layer.\nBlocks tab: every block the structure needs; tap a "
     "row to tick it off."},
    {"Substitute a block",
     "Use another block where the type doesn't matter, e.g. cobblestone for 500 smooth stone.\n"
     "In the panel's Blocks tab, tap ⋯ on a row. Type part of a name and tap a result, or tap Held block and "
     "then tap any block while holding the replacement (that tap places nothing).\nEvery block of that type is "
     "replaced: hologram, Easy Place and counts follow. Substituted rows show *; tap ⋯ again for Undo. "
     "The structure file is not changed. Substitutions stay until you load another structure."},
    {"Lock and saved placement",
     "The structure, its position, rotation, mirror and layer view are saved whenever they change, and come "
     "back when you restart the game or rejoin.\nThe padlock in the panel header locks the placement: moving, "
     "rotating, mirroring and Move hologram to me are blocked until you unlock it."},
    {"Easy Place",
     "Turn it on in the Easy Place tab. Hold the block the structure wants, aim at its ghost block and tap: "
     "it is placed exactly there. Holding the wrong block tells you which one to hold.\nWater and lava: "
     "use a bucket and aim at the spot; the game pours it as usual."},
    {"Hologram look",
     "Hologram opacity, Block outlines and Tinted fill change how ghost blocks are drawn. Show hologram hides "
     "it without unloading the structure."},
    {"Where files go",
     "Imported structures are kept in Android/data/<launcher>/files/BE-Matica/structures. Tap Refresh list "
     "after adding files there yourself."},
}};

inline constexpr std::array<Topic, 7> kSpanish{{
    {"Primeros pasos",
     "1. En la pestaña BE-Matica, toca Import structure y elige un .mcstructure (o un .litematic de Java, que "
     "se convierte).\n2. Eligélo en Structure. El holograma aparece con su esquina donde estás parado.\n"
     "3. Construye dentro de los bloques fantasma azules. Los bloques que colocas bien desaparecen del holograma."},
    {"Panel de control",
     "El panel flotante se ve en pantalla mientras BE-Matica Controls esté activado. Arrastra su encabezado "
     "(los puntos) para moverlo; la flecha lo pliega.\nEl encabezado muestra cuánto de la estructura ya está "
     "construido.\nPestaña Move: la cruceta mueve el holograma un bloque hacia donde miras (arriba = adelante, "
     "izquierda = tu izquierda), el par ▲ / ▼ lo sube y lo baja, Rotate +90° lo gira, Mirror lo "
     "voltea, < y > eligen una sola capa, All muestra todas las capas.\nPestaña Blocks: todos los bloques que "
     "necesita la estructura; toca una fila para marcarla."},
    {"Sustituir un bloque",
     "Usa otro bloque cuando el tipo no importa, por ejemplo adoquín en lugar de 500 de piedra lisa.\n"
     "En la pestaña Blocks del panel, toca ⋯ en una fila. Escribe parte del nombre y toca un resultado, "
     "o toca Held block y luego toca cualquier bloque mientras sostienes el reemplazo (ese toque no coloca "
     "nada).\nSe reemplazan todos los bloques de ese tipo: el holograma, Easy Place y los conteos lo siguen. Las "
     "filas sustituidas muestran *; toca ⋯ otra vez para Undo. El archivo de la estructura no cambia. Las "
     "sustituciones duran hasta que cargues otra estructura."},
    {"Bloqueo y posición guardada",
     "La estructura, su posición, rotación, espejo y vista de capas se guardan cada vez que cambian, y "
     "regresan cuando reinicias el juego o vuelves a entrar.\nEl candado del encabezado del panel bloquea la "
     "posición: mover, rotar, voltear y Move hologram to me quedan bloqueados hasta que lo desbloquees."},
    {"Easy Place",
     "Actívalo en la pestaña Easy Place. Sostén el bloque que pide la estructura, apunta a su "
     "bloque fantasma y toca: se coloca justo ahí. Si sostienes el bloque equivocado, te dice cuál "
     "debes sostener.\nAgua y lava: usa una cubeta de agua o de lava y apunta al lugar; el juego la vacía "
     "como siempre."},
    {"Aspecto del holograma",
     "Hologram opacity, Block outlines y Tinted fill cambian cómo se dibujan los bloques fantasma. Show "
     "hologram lo oculta sin descargar la estructura."},
    {"Dónde van los archivos",
     "Las estructuras importadas se guardan en Android/data/<launcher>/files/BE-Matica/structures. Toca Refresh "
     "list después de agregar archivos ahí por tu cuenta."},
}};

inline constexpr std::array<Topic, 7> kPortuguese{{
    {"Primeiros passos",
     "1. Na aba BE-Matica, toque em Import structure e escolha um .mcstructure (ou um .litematic do Java, que "
     "é convertido).\n2. Escolha-o em Structure. O holograma aparece com o canto onde você está.\n"
     "3. Construa dentro dos blocos fantasma azuis. Os blocos que você coloca certo somem do holograma."},
    {"Painel de controle",
     "O painel flutuante fica na tela enquanto BE-Matica Controls estiver ligado. Arraste o cabeçalho (os "
     "pontos) para movê-lo; a seta o recolhe.\nO cabeçalho mostra quanto da estrutura já foi "
     "construído.\nAba Move: o direcional move o holograma um bloco para onde você olha (cima = frente, "
     "esquerda = sua esquerda), o par ▲ / ▼ sobe e desce, Rotate +90° gira, Mirror espelha, < e > "
     "escolhem uma única camada, All mostra todas.\nAba Blocks: todos os blocos que a estrutura precisa; "
     "toque em uma linha para marcá-la."},
    {"Trocar um bloco",
     "Use outro bloco quando o tipo não importa, por exemplo pedregulho no lugar de 500 de pedra lisa.\n"
     "Na aba Blocks do painel, toque em ⋯ em uma linha. Digite parte do nome e toque em um resultado, ou "
     "toque em Held block e depois em qualquer bloco segurando o substituto (esse toque não coloca "
     "nada).\nTodos os blocos desse tipo são trocados: holograma, Easy Place e contagens acompanham. As "
     "linhas trocadas mostram *; toque em ⋯ de novo para Undo. O arquivo da estrutura não muda. As "
     "trocas valem até você carregar outra estrutura."},
    {"Trava e posição salva",
     "A estrutura, a posição, a rotação, o espelho e a vista de camadas são salvos "
     "sempre que mudam, e voltam quando você reinicia o jogo ou entra de novo.\nO cadeado no cabeçalho "
     "do painel trava a posição: mover, girar, espelhar e Move hologram to me ficam bloqueados até "
     "você destravar."},
    {"Easy Place",
     "Ligue na aba Easy Place. Segure o bloco que a estrutura pede, mire no bloco fantasma e toque: ele é "
     "colocado exatamente ali. Se você segurar o bloco errado, o jogo avisa qual segurar.\nÁgua e "
     "lava: use um balde de água ou de lava e mire no lugar; o jogo despeja como de costume."},
    {"Visual do holograma",
     "Hologram opacity, Block outlines e Tinted fill mudam como os blocos fantasma são desenhados. Show "
     "hologram o esconde sem descarregar a estrutura."},
    {"Onde ficam os arquivos",
     "As estruturas importadas ficam em Android/data/<launcher>/files/BE-Matica/structures. Toque em Refresh "
     "list depois de adicionar arquivos lá por conta própria."},
}};

constexpr std::span<const Topic> topics(Lang lang) {
  return lang == Lang::Spanish ? std::span<const Topic>(kSpanish)
         : lang == Lang::Portuguese ? std::span<const Topic>(kPortuguese)
                                    : std::span<const Topic>(kEnglish);
}

constexpr const char *languageLabel(Lang lang) {
  return lang == Lang::Spanish ? "Idioma" : lang == Lang::Portuguese ? "Idioma" : "Language";
}

}  // namespace help
