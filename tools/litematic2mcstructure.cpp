// Desktop converter: litematic2mcstructure input.litematic [output.mcstructure]
#include "litematic.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s input.litematic [output.mcstructure]\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  bematica::Structure s;
  bematica::LitematicInfo info;
  std::string error;
  if (!bematica::loadLitematic(data, s, info, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::printf("%s by %s: %d region(s), %d x %d x %d, %zu Bedrock block states\n", info.name.c_str(),
              info.author.c_str(), info.regions, s.size[0], s.size[1], s.size[2], s.palette.size());
  for (const auto &name : info.unknownBlocks) std::printf("  no Bedrock block for %s (left out)\n", name.c_str());
  std::string outPath = argc > 2 ? argv[2] : std::string(argv[1]);
  if (argc <= 2) outPath = outPath.substr(0, outPath.rfind('.')) + ".mcstructure";
  const auto bytes = bematica::saveMcstructure(s);
  std::ofstream(outPath, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  std::printf("wrote %s\n", outPath.c_str());
  return 0;
}
