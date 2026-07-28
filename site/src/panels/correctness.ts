import { MUTATIONS, MUTATION_CITATION, PROJECT } from '../content'
import { element, integer, scrollBox, statTable } from '../format'

// The correctness story. Not per symbol, and given real estate on purpose: it is
// the strongest artifact in the project.

function mutationTable(): HTMLElement {
  const body = element('tbody')
  for (const row of MUTATIONS) {
    body.append(
      element(
        'tr',
        {},
        element('td', {}, row.injected),
        element('td', { class: 'good' }, row.caught),
        element('td', { class: 'num' }, integer(row.command)),
      ),
    )
  }

  return element(
    'table',
    { class: 'grid' },
    element(
      'thead',
      {},
      element(
        'tr',
        {},
        element('th', { scope: 'col' }, 'Bug injected on purpose'),
        element('th', { scope: 'col' }, 'Caught by'),
        element('th', { scope: 'col', class: 'num' }, 'At command'),
      ),
    ),
    body,
  )
}

export function correctnessPanel(): HTMLElement {
  return element(
    'section',
    { class: 'panel', id: 'correctness' },
    element('h2', {}, 'Correctness'),

    element(
      'p',
      { class: 'lede' },
      'Every randomised command goes to both the real engine and a deliberately naive std::map plus std::list reference book, ' +
        'and full state equivalence is asserted after every single command: best bid, best ask, per level aggregate quantity, ' +
        'per level order count, the exact ordered sequence of order ids at every occupied level, and the emitted event stream. ' +
        'Comparing after each command rather than at the end means a mismatch names the exact command, with a book small enough to read. ' +
        'The id sequence check is the one that matters most, because aggregates can agree while queue order is wrong, and queue order is what price time priority actually promises.',
    ),

    element('h3', {}, 'A differential test that has never failed is one nobody should trust'),
    element(
      'p',
      {},
      'So three bugs were injected deliberately, and each was caught by a different one of the comparison’s checks. ' +
        'Each check earns its place, which is the point of running the exercise rather than assuming it.',
    ),
    scrollBox(mutationTable()),
    element('p', { class: 'citation' }, `${MUTATION_CITATION.document}, "${MUTATION_CITATION.section}"`),

    element('h3', {}, 'What runs, and where'),
    statTable([
      ['Tests', `${integer(PROJECT.tests)}, green on ${PROJECT.compilers} across ${PROJECT.presets}`],
      ['CI jobs', `${integer(PROJECT.ciJobs)}, on ${PROJECT.ciCompilers}`],
      ['Sanitizers', `${PROJECT.sanitizers}, every one on every push`],
      [
        'Fuzzing',
        'Coverage guided differential fuzzing, short on every push and long on the nightly schedule. This target cannot be built on the development host at all, so CI is the only place it exists.',
      ],
      [
        'Differential budget',
        'A million randomised commands per push, ten million nightly. A bug found at any budget is shrunk to a minimal reproducer, committed, and replayed on every push from then on, which is what makes the reduced push budget safe.',
      ],
      [
        'Real data',
        `Validated against a real NASDAQ TotalView capture, ${PROJECT.captureDate}, ${PROJECT.captureMessages} messages across ${PROJECT.captureSymbols} symbols.`,
      ],
      [
        'Artifact guard',
        'The QQQ artifact set on this page is regenerated from the committed slice and diffed byte for byte on every push, so these numbers cannot drift from the code.',
      ],
    ]),

    element('h3', {}, 'The bug log'),
    element(
      'p',
      {},
      `DESIGN.md carries ${integer(PROJECT.bugLogEntries)} entries describing bugs the tests caught, several of them self-inflicted, ` +
        'because a test suite that never caught anything is not evidence that the code is correct, only that the tests are weak. ' +
        'The most instructive one: fill-or-kill undercounted liquidity resting in cold storage, because the precheck walked only the band bitmap while the matcher walked cold levels too. ' +
        'It was demonstrated both ways. Reverted, a new differential configuration fails at command 154 while all three pre-existing differential tests pass, which was exactly the blind spot.',
    ),
  )
}
