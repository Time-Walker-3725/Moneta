# Accounting book

A `Book` is a single-writer C++ object. Open one cash account for each wallet or bank account. An account's currency includes its minor-unit scale. A currency code uses one scale throughout a book. IDs start at one, correspond to insertion order, and survive a save and reload. The collection accessors expose read-only records for building an account or schedule picker.

## Posting money

| Action | Effect |
| --- | --- |
| `opening` | Sets the starting amount through the first journal entry affecting the account. A payable opens at zero or below, a receivable at zero or above. |
| `income` | Credits positive income to a cash account. |
| `spend` | Debits cash and creates a linked purchase in that currency's tree. The optional category, tags and location classify the expense. |
| `transfer` | Debits the source and credits the destination. Same-currency amounts must match. For FX, pass the actual settled amounts. |
| `principal` | Transfers principal between a cash account and a receivable or payable in the same currency. |
| `invest`, `redeem` | Debits invested principal, then credits principal and earned interest once at redemption. |

Cash may be overdrawn. A receivable's positive balance is the amount owed to you. A payable's negative balance is the amount you owe. Borrowing moves principal from payable to cash. Repayment reverses the direction. Lending moves principal from cash to receivable. Collection reverses that direction. Overpaying an obligation throws before either balance changes. Record interest paid as an expense and interest collected as income.

Posting order determines balance validation. Dates determine reporting and interest accrual. The journal retains both transfer legs and their account IDs. `income_total` includes posted income and realized investment interest for the selected account and inclusive dates. Opening balances, transfers and principal movements are excluded.

## FX and fixed-interest investments

`ExchangeRate{usd, jpy, 150, 100}.convert(Money{101, usd})` converts 101 US cents to 152 yen at 150 yen per dollar. The ratio is in target and source **minor units**. Conversion rounds once, half up. Passing the actual received amount directly to `transfer` also supports settlement at a different effective rate.

An investment has a cash account, original amount, funding date, label and nonnegative fixed annual rate in basis points. For example, 525 means 5.25% per year. Simple interest in minor units equals principal × rate × elapsed calendar days ÷ 3,650,000, rounded once, half up. Funding day earns zero interest. Leap days count as elapsed days and the denominator stays 365. `interest` quotes accrued interest. Redemption realizes it and ends accrual. Principal and interest remain separately inspectable.

## Receipts, reports and existing files

Use `entry(payment).purchase` to find a receipt in `expenses(currency)`. To classify or split posted spending, copy that tree, apply the existing tree editing API and pass the result to `replace_expenses`. The book validates amount, account, date, location and journal link for each purchase before replacement. Posted amounts are immutable in the journal. The underlying standalone tree retains its original editing API.

`spending_by_location(currency, scope, filter)` groups matching leaves by owning purchase location. The empty optional key groups purchases without a location. A tagged component contributes only its own amount. Different currency trees have independent totals.

To migrate a version-1 standalone tree, load it with `load_tree_file`, register cash accounts and locations corresponding to its numeric IDs, then call `book.import_expenses(tree)` on a fresh hierarchy for that currency. The import preserves receipt IDs, order, splits, tags, merchant text and notes. It posts one debit per purchase and assigns book-local journal IDs. Starting balances should represent balances **before** the imported spending.

## Recurrence and local persistence

Schedules post income or expenses daily, weekly, monthly or yearly, starting on the supplied date. Monthly and yearly occurrences use the original day or the last valid day of a shorter month. A January 31 schedule therefore returns to March 31 after February. Call `run_due(id, through, limit)` to advance a schedule through an inclusive cutoff. Repeating a call resumes from its saved next date. Each successful occurrence posts and advances together. If a later posting fails, earlier successes remain in the book. `cancel` stops future occurrences.

Use `BookDatabase::edit` to persist a complete user action immediately. A schedule run wrapped in one edit commits as one revision. An exception that leaves the callback rolls the entire action back. `Book::save` creates SQLite files for detached snapshots. `Book::load` recognizes SQLite and version-1 text files. Saving to an existing version-1 file preserves its format. See the [SQLite storage guide](sqlite.md).
