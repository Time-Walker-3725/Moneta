# Moneta

Moneta is a planned accounting application written entirely in C++. Its backend will be fully C++ based, with a focus on correctness, reliable persistence, and clear interfaces before frontend development.

Trees will be the primary data structure for organizing expenses and calculating expense totals. Income, accounts, debts, and investments will have separate models; income does not belong in the expense tree.

## Transaction tree

Users can record a simple transaction first, then refine its categories or split it into more detailed items later.

For example, an expense can begin as **Food: 198 INR**, later become **Food → Dominos: 198 INR**, and finally be split into **Pizza: 150 INR** and **Taco: 48 INR** under the same Dominos transaction.

```text
Expenses                         198 INR
└── Food                         198 INR
    └── Dominos                  198 INR
        └── Transaction          198 INR
            ├── Pizza            150 INR
            └── Taco              48 INR
```

Food can contain multiple restaurant nodes, and each restaurant can contain multiple transactions. Transactions are tree nodes: an unsplit transaction is a leaf, while a split transaction becomes a parent of its item nodes.

## Amounts and aggregation

- A leaf stores an individual amount.
- A parent node's value is the sum of its immediate children's values, recursively representing the total of all leaves in its subtree.
- Splitting a transaction preserves its total: 150 INR + 48 INR = 198 INR.
- Parent totals and child amounts must not be counted twice. A split replaces the original undivided amount with its component amounts.

This lets the same tree provide totals for all expenses, a category such as Food, a restaurant such as Dominos, or an individual transaction.

## Transaction metadata

Every transaction has a date attached as metadata. Users can also provide optional location data. Refining categories or splitting an existing transaction must preserve its accounting context, including its date and any supplied location.

## Totals over time

The application must support calculating money spent during a selected date range, both across expenses and within a chosen subtree.

The target query complexity is **O(1) to O(n)**, where `n` is the number of relevant transaction or item leaves examined. A direct traversal can calculate a range total in O(n). Faster queries will require suitable date indexes or cached aggregates; a cached subtree total alone does not provide O(1) queries for arbitrary date ranges. The indexing and caching strategy remains to be designed.

## Planned capabilities

- **Spending reports:** View overall expenses, monthly expenses, and expenses within any selected date range, including totals for individual categories or subtrees.
- **Location reports:** Track where money is spent and filter location totals by date range.
- **Accounts and balances:** Manage cash and bank accounts, opening balances, deposits, withdrawals, and transfers; calculate current balances from recorded ledger entries.
- **Income:** Record incoming money separately from expenses.
- **Debtors and creditors:** Track money owed to the user and money the user owes others, including partial repayments and outstanding balances.
- **Investments:** Record stock purchases and sales, track holdings, and calculate portfolio value using dated price quotes. Return calculations should account for purchase costs, fees, and dividends.
- **Deposit assets:** Support fixed deposits or similar interest-bearing assets, tracking principal, interest, maturity, and valuation. These differ from depositing money into an account, which is a cash movement.
- **Map visualization, in a later phase:** Display bubbles or indicators showing where money was spent and how much, with date-range filtering.

For the map, the backend should return location summaries containing coordinates, amount spent, currency, and transaction count for the requested period. The frontend can render those summaries without calculating financial totals itself.

## Accounting boundaries

Money leaving an account is not always an expense. Transfers between the user's accounts, stock purchases, money lent to someone, and repayments of loan principal have different accounting meanings.

The ledger records financial movements and supports account balances. The expense tree classifies actual expenses. An expense references the account that paid for it, so recording it must update the relevant financial records consistently without double counting.

## Proposed file structure

The following structure is a plan, not a list of files already implemented. Create modules as their features are developed rather than adding empty placeholders.

```text
Moneta/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── docs/
│   ├── architecture.md
│   └── accounting-rules.md
├── include/moneta/
│   ├── core/
│   │   ├── money.hpp
│   │   ├── identifiers.hpp
│   │   └── date_range.hpp
│   ├── accounts/
│   │   ├── account.hpp
│   │   └── ledger_entry.hpp
│   ├── expenses/
│   │   ├── expense.hpp
│   │   ├── expense_tree.hpp
│   │   └── expense_split.hpp
│   ├── income/
│   │   └── income.hpp
│   ├── locations/
│   │   └── location.hpp
│   ├── debts/
│   │   ├── obligation.hpp
│   │   └── repayment.hpp
│   ├── investments/
│   │   ├── trade.hpp
│   │   ├── price_quote.hpp
│   │   └── portfolio.hpp
│   ├── reports/
│   │   ├── spending_report.hpp
│   │   ├── location_report.hpp
│   │   └── net_worth_report.hpp
│   └── application/
│       └── finance_service.hpp
├── src/
│   ├── accounts/
│   ├── expenses/
│   ├── income/
│   ├── locations/
│   ├── debts/
│   ├── investments/
│   ├── reports/
│   ├── application/
│   └── storage/
├── apps/
│   └── cli/
│       └── main.cpp
└── tests/
    ├── unit/
    └── integration/
```

Headers under `include/moneta/` describe public interfaces, with corresponding `.cpp` implementations under `src/`.

| Module | Responsibility |
| --- | --- |
| `core` | Shared money, identifier, and date-range types. |
| `accounts` | Accounts, ledger entries, transfers, and balance calculations. |
| `expenses` | Expense categories, dated purchases, the expense tree, and splits. |
| `income` | Incoming money recorded outside the expense tree. |
| `locations` | Place names, addresses, and optional coordinates attached to expenses. |
| `debts` | Obligations, repayments, and outstanding amounts. |
| `investments` | Trades, holdings, dated prices, and portfolio calculations. |
| `reports` | Spending, location, debt, portfolio, and net worth summaries. |
| `application` | Operations that coordinate modules, such as recording an expense or repaying a debt. |
| `storage` | Persistence, migrations, and atomic updates; financial rules remain in the financial modules. |
| `apps/cli` | A command-line interface for exercising the backend before frontend development. |
| `tests` | Unit tests for financial rules and integration tests for persistence and coordinated operations. |

## Development roadmap

1. Build shared types, accounts, the ledger, expenses, income, persistence, and date-range reports.
2. Validate a complete workflow through the CLI: create an expense, categorize it, split it, move it, query totals by date, then save and reload it.
3. Add location reports, debts and repayments, and investment tracking.
4. Build the frontend against stable backend interfaces, with charts and map visualization in later phases.

A central invariant is that rearranging or refining the expense tree must never accidentally create or lose money. Reliable backend interfaces will simplify frontend work, while transaction editing, charts, and maps will still require their own design and validation.

## Additional feature ideas

These are suggestions for future consideration, rather than commitments for the initial backend:

- Category budgets with monthly progress.
- Recurring expenses, recurring income, and payment reminders.
- Search and filters by merchant, amount, date, category, and location.
- CSV import/export, backups, and reliable restoration.
- Receipt attachments and transaction notes.
- Reconciliation against bank statements.
- Debt due dates and repayment reminders.
- Net worth history and portfolio gain/loss reports.
- Multiple currencies with explicit exchange rates.
- An edit history for reviewing and correcting changes.

## Project status

The project is currently at the requirements stage. No application code, build system, or tests have been added yet.
