# Card Number Passwords 1.2.0

Para **YFM Re-Decomp Mod API 9 + added-card password shop update**. Actualización del mod 1.0.0 de Douglas.

## Compatibilidad con otros mods

Las contraseñas explícitas de otros mods tienen prioridad sobre los números
de carta. Admite `cards[].password` y la tabla `passwords` de FM Editor,
incluidos los precios de la PR #250. La tabla `passwords` prevalece sobre
`cards[].password` de la misma carta. Si hay contraseñas explícitas duplicadas,
gana el ID menor. Las entradas finales dependen del orden de carga de los mods.

Aurora Wing con `replace: 58` conserva el ID **58**. La contraseña
**00000723** encuentra Aurora Wing por **100 StarChips**, aunque otro mod
añada una carta con ID 723. **00000058** también funciona como alias.
Una contraseña vacía/null desactiva también el alias numérico de esa carta.

Las cartas sin contraseña explícita de un mod usan su ID actual con ocho
dígitos: 722 = `00000722`, 1500 = `00001500`. Las cartas añadidas se detectan
durante la ejecución. Este mod no restaura las contraseñas impresas originales. Si una contraseña
explícita ocupa el alias de otra carta, ese alias no se muestra en esa otra
carta; asígnale una contraseña única para poder comprarla.

## Instalación

Reemplaza la carpeta `mods/card-number-passwords`, reinicia el juego y activa
**Card Number Passwords** en **Game > Mods**. Conserva tu `config.ini` si ya
personalizaste los precios. Los IDs de cartas añadidas dependen del orden
de los mods que las añaden.

## Precios en StarChips

Edita `config.ini` antes de iniciar:

- `stock_cards_default = -1`: conserva el precio cargado de las cartas 1–722,
  incluidos otros mods y el campo Starchips de FM Editor. El juego aplica
  los porcentajes una sola vez.
- Un `stock_cards_default` no negativo sustituye esos precios.
- `added_cards_default = 999999`: precio predeterminado para cartas superiores
  a 722. Las reglas de precio de `passwords` tienen prioridad sobre este valor.
- En `[cards]`, `ID = precio` sustituye los valores predeterminados. Admite
  0–999999; 0 es gratis. Si se repite un ID, gana la última entrada.

La configuración incluida conserva el ajuste original `722 = 10`. Usa el
ID de carta, no la contraseña: Aurora Wing sería `58 = 100`, no `723 = 100`.

## Tienda y compras

Se conservan la navegación y las contraseñas de paquetes, y la comprobación
de capacidad del baúl. Las cartas 1–722 permiten una compra por ID,
compartida por sus aliases. Las cartas añadidas pueden comprarse repetidamente.
Las contraseñas/aliases de cartas tienen prioridad sobre las de paquetes.
View > Card passwords muestra la misma contraseña que usa la tienda.
Esta versión exige la actualización del juego que añade `Cards_PasswordPrice`;
no funciona en la v0.2.0 original. Otros mods que alteren las mismas funciones
de contraseña/precio todavía pueden entrar en conflicto.

## Compilar

Desde la instalación del juego:

```powershell
python sdk/tools/build_mod.py ruta/a/card-number-passwords
```

El `.o` incluido es multiplataforma (Linux y Windows), compilado con el SDK del juego.
