# La Guasabara: núcleo

El código del juego de La Guasabara, una liga privada y sin fines de lucro de *Super Street Fighter II X* (`ssf2xjr1`). Se publica porque la licencia de FB Neo pide hacer públicos los cambios a su código.

> *The game code of La Guasabara, a private non-profit Super Street Fighter II X league: a narrow API over FB Neo's CPS2 core (Fightcade's fork, pinned), GekkoNet rollback netplay, an SDL3 game window and a relay-aware UDP transport. Published because FB Neo's license asks for changes to be public.*

**No trae ningún juego.** Cada jugador usa sus propios archivos (`ssf2xjr1.zip` y `ssf2t.zip`). Aquí no hay ROMs ni se descargan.

## Qué hay

| Carpeta | Qué es |
|---|---|
| `spike/patches/` | **El único cambio al código de FB Neo:** `0001-qsound-resampler-state.patch` guarda en el estado la fase del remuestreador de QSound, sin la cual cargar un estado no da exactamente lo mismo (hace falta para el *rollback*). |
| `spike/harness/build.py` | Baja el fork de Fightcade (`fightcade-fbneo`, commit `c95950140e2424643a0bb589dc444f0defe03155`) y GekkoNet (commit `0d491a4524e10173fc5d2aae22d042d3ae9f2d44`), aplica el parche y compila con clang. |
| `core/` | `guasa-core`: una interfaz estrecha sobre el núcleo CPS2 (iniciar, avanzar un cuadro, guardar y cargar, hashes, leer RAM, imagen y sonido) y un estado inicial igual en todas las PCs. Lee el juego directo de los `.zip`. |
| `game/` | La ventana del juego (SDL3) y un bot sin ventana. Tiene versus local, Test Game, controles con la regla de direcciones opuestas y partidas en línea con *rollback* (GekkoNet). El transporte UDP mide la ruta directa y la del relay del servidor, y usa la más rápida. |

## Compilar

Se necesita Python 3 y clang.

- **Windows:** el [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) de 2026-09-22 (`llvm-mingw-20260922-ucrt-x86_64`) en `spike/tools/`.
- **Linux:** el clang del sistema.
- **La ventana del juego (solo Windows):** también [SDL3](https://github.com/libsdl-org/SDL) 3.4.18 para MinGW, en `spike/tools/SDL3-3.4.18/`.

```
python -I core/build.py      # el núcleo y su programa de pruebas (spike/build/core_test)
python -I game/build.py      # el bot (Windows y Linux) y la ventana (Windows)
```

Los dos bajan el fork y GekkoNet la primera vez, y les aplican `spike/patches/`.

## Probar

Con los archivos del juego en una carpeta (sueltos o en sus `.zip`):

```
spike/build/core_test <carpeta> check            # XXH64, estado inicial y velocidad
spike/build/core_test <carpeta> rollback 1800    # rollback de 7 cuadros en cada cuadro
spike/build/core_test <carpeta> hashes 3600      # una línea de hashes por cuadro
```

`core/tests/expected.txt` tiene lo que tiene que dar cualquier PC:
- el estado inicial `450f108a23a6c503`;
- el SHA-256 de los 3600 cuadros de `hashes`.

Dio lo mismo en Windows, en Linux y en SteamOS.

Contra el emulador de Fightcade, 105 000 cuadros dieron la RAM idéntica. Esa comparación se hizo con el emulador instalado y no está aquí.

## Licencias

- **El código de FB Neo** (que el build baja del fork) y **el parche** de `spike/patches/` están bajo la licencia de FB Neo, copiada en [LICENSE-FBNeo.txt](LICENSE-FBNeo.txt). Esa licencia incluye la de MAME por referencia y no permite ningún uso con fines de lucro.
- **GekkoNet:** licencia BSD de 2 cláusulas.
- **SDL3 y zlib:** licencia zlib.
- **El resto de este repositorio** (`core/`, `game/` y los scripts) es de La Guasabara, bajo la licencia MIT ([LICENSE](LICENSE)).
- Un programa compilado con todo esto queda bajo las condiciones de FB Neo: sin vender, sin pedir donaciones, con su licencia y con estos cambios públicos.

"Street Fighter" y "Capcom" son marcas de sus dueños. Este proyecto no tiene relación con Capcom ni con Fightcade.
