# Card Number Passwords 1.2.0

Para **YFM Re-Decomp Mod API 9 + added-card password shop update**. Atualização do mod 1.0.0 de Douglas.

## Compatibilidade com outros mods

Passwords definidos por outros mods têm prioridade sobre os números das
cartas. Funciona com `cards[].password` e com a tabela `passwords` do
FM Editor, inclusive os preços da PR #250. A tabela `passwords` prevalece
sobre `cards[].password` da mesma carta. Se duas cartas tiverem o mesmo
password explícito, vence o menor ID. Vale a ordem de carregamento dos mods.

No exemplo Aurora Wing, `replace: 58` mantém o ID **58**. O password
**00000723** encontra Aurora Wing por **100 StarChips**, mesmo se outro mod
adicionar uma carta com ID 723. **00000058** também funciona como alias.
Um password vazio/null desativa também o alias numérico daquela carta.

Cartas sem password explícito de mod usam seu ID atual com oito dígitos:
722 = `00000722`, 1500 = `00001500`. Cartas adicionadas são detectadas durante
a execução. Este mod não restaura os passwords impressos das cartas originais. Se um
password explícito ocupar o alias de outra carta, esse alias não aparece
nessa outra carta; defina um password único para poder comprá-la.

## Instalar

Substitua a pasta `mods/card-number-passwords`, reinicie o jogo e ative
**Card Number Passwords** em **Game > Mods**. Preserve seu `config.ini` se
você já personalizou os preços. Os IDs de cartas adicionadas dependem da
ordem dos mods que as adicionam.

## Preços em StarChips

Edite `config.ini` antes de iniciar:

- `stock_cards_default = -1`: mantém o preço carregado das cartas 1–722,
  incluindo outros mods e o campo Starchips do FM Editor. Porcentagens são
  aplicadas uma única vez pelo jogo.
- Um `stock_cards_default` não negativo substitui esses preços.
- `added_cards_default = 999999`: padrão para cartas acima de 722. As regras de preço
  na tabela `passwords` têm prioridade sobre esse padrão.
- Em `[cards]`, `ID = preço` substitui os padrões. Aceita 0–999999; 0 é grátis.
  A última linha repetida para um ID vence.

O arquivo incluído mantém a configuração original `722 = 10`. Use o ID,
não o password: para Aurora Wing, seria `58 = 100`, não `723 = 100`.

## Loja e compras

A navegação e os passwords de pacotes continuam disponíveis, assim como a
verificação de espaço no baú. Cartas 1–722 continuam limitadas a uma compra
por ID, compartilhada entre os aliases. Cartas adicionadas podem ser
compradas novamente. Passwords/aliases de cartas têm prioridade sobre
passwords de pacotes. View > Card passwords mostra o mesmo password usado pela loja.
Esta versão exige a atualização do jogo que adiciona `Cards_PasswordPrice`;
não funciona na v0.2.0 original. Outros mods que alteram as mesmas funções
de password/preço ainda podem entrar em conflito.

## Compilar novamente

Na raiz da instalação do jogo:

```powershell
python sdk/tools/build_mod.py caminho/para/card-number-passwords
```

O `.o` incluído é multiplataforma (Linux e Windows), compilado com o SDK do jogo.
