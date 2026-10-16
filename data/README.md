# IWAD (dados do jogo)

O firmware **não inclui** o arquivo WAD. Coloque um destes na **raiz** do cartão microSD (FAT32):

| Arquivo | Origem |
| --- | --- |
| `doom1.wad` | Doom shareware (redistribuível) |
| `doom.wad` | Doom registrado (compre o jogo) |
| `doom2.wad` | Doom II (compre o jogo) |
| `freedoom1.wad` / `freedoom2.wad` | [Freedoom](https://freedoom.github.io/) (livre) |

Para baixar o shareware:

```bash
./tools/download_shareware_wad.sh
```

Depois copie `data/doom1.wad` para o cartão.

**Na hora de gravar o firmware pelo USB, retire o cartão SD.** No VGA32 v1.4 o pino MOSI do SD é o GPIO12, que também é strapping pin do ESP32.
