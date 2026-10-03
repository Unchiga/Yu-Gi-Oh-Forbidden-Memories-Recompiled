# Card Number Passwords 1.2.0

For **YFM Re-Decomp Mod API 9 + added-card password shop update**. Updated from Douglas's 1.0.0 mod.

## Passwords from other mods

Explicit passwords from other mods take priority over card-number aliases.
This includes both `cards[].password` and the `passwords` table written by
FM Editor (including PR #250). A `passwords` table entry takes precedence
over the same card's `cards[].password`. Duplicate explicit passwords give
the lowest card ID. The current mod load order determines the final entries.

For example, Aurora Wing with `replace: 58`, password `00000723`, and
`passwords.58.starchips: 100` is card **58**. Entering **00000723** finds
Aurora Wing for **100 StarChips**, even if another mod adds a real card 723.
The numeric alias **00000058** also works. An explicit empty/null password
disables that card's numeric alias too.

Cards without an explicit mod password use their current ID as eight digits:
card 722 is `00000722`, card 1500 is `00001500`. Added cards are discovered at
runtime. Retail printed passwords are not restored by this mod. If an explicit
password occupies another card's numeric alias, the viewer does not print that
alias on the other card. Give that card a unique explicit password to sell it.

## Installation

Replace the existing `mods/card-number-passwords` folder with this folder,
restart the game, and enable **Card Number Passwords** in **Game > Mods**.
If you have customized `config.ini`, keep your copy. IDs for added cards
follow the load order of the mods that add them.

## StarChip prices

Edit `config.ini` before starting the game:

- `stock_cards_default = -1` keeps the loaded price for cards 1–722, including
  other mods' prices and FM Editor's Starchips field. Percentage rules are
  applied once by the game.
- A nonnegative `stock_cards_default` overrides those loaded prices.
- `added_cards_default = 999999` is the default for cards beyond 722; their
  manifest `passwords` price rules take precedence over this default.
- Under `[cards]`, `ID = price` overrides either default. Prices are 0–999999;
  0 is free. If an ID is repeated, the last entry wins.

The included configuration retains the original `722 = 10` override.
Per-card INI prices always use the card ID, not its password: Aurora Wing's
price override would be `58 = 100`, not `723 = 100`.

## Compatibility

The shop retains pack browsing/password handling and chest-capacity checks.
Cards 1–722 retain their one-purchase flag by card ID, shared by all aliases.
Added cards can be bought repeatedly because they have no retail used flag.
Card passwords/aliases take priority over pack passwords, as cards do in the
normal shop. This mod hooks the shared password/price policy and lookup. The normal shop
handles purchases. View > Card passwords shows the same preferred password.
This version requires the game update that adds `Cards_PasswordPrice`; it
does not run on unmodified v0.2.0. Other mods hooking these policies can conflict.

## Rebuilding

From the game installation directory:

```powershell
python sdk/tools/build_mod.py path/to/card-number-passwords
```

The supplied `.o` is built for both Linux and Windows using the game's SDK.
