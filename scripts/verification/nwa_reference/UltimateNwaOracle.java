// Test adapter only. All automata operations below execute the user's unchanged
// Ultimate source checkout. No Ultimate implementation is vendored here.
import java.io.BufferedInputStream;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Scanner;
import java.util.Set;

import de.uni_freiburg.informatik.ultimate.automata.AutomataLibraryServices;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.INwaOutgoingLetterAndTransitionProvider;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.NestedWord;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.NestedWordAutomaton;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.VpAlphabet;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.buchi.BuchiIsEmpty;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.Accepts;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.Complement;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.Determinize;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.Difference;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.Intersect;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.IsEmpty;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.RemoveDeadEnds;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.simulation.util.nwa.graph.summarycomputationgraph.ReduceNwaDirectSimulationB;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.operations.simulation.util.nwa.graph.summarycomputationgraph.ReduceNwaDelayedSimulationB;
import de.uni_freiburg.informatik.ultimate.automata.nestedword.reachablestates.NestedWordAutomatonReachableStates;
import de.uni_freiburg.informatik.ultimate.automata.statefactory.StringFactory;
import de.uni_freiburg.informatik.ultimate.core.model.services.ILogger;
import de.uni_freiburg.informatik.ultimate.core.model.services.IUltimateServiceProvider;

public final class UltimateNwaOracle {
    @SuppressWarnings("unchecked")
    private static <T> T service(Class<T> type) {
        return (T) Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, (p, m, a) -> {
            if (m.getName().equals("getLogger")) return ILogger.getDummyLogger();
            if (m.getName().equals("continueProcessing")) return true;
            if (m.getName().equals("toString")) return "NWA oracle services";
            if (m.getName().equals("hashCode")) return System.identityHashCode(p);
            if (m.getName().equals("equals")) return p == a[0];
            Class<?> result = m.getReturnType();
            if (result == boolean.class) return false;
            if (result == int.class) return 0;
            if (result == long.class) return 0L;
            if (result == void.class) return null;
            if (result.isInterface()) return service(result);
            return null;
        });
    }

    private static final AutomataLibraryServices SERVICES =
        new AutomataLibraryServices(service(IUltimateServiceProvider.class));
    private static final StringFactory FACTORY = new StringFactory();

    private static NestedWordAutomaton<Integer, String> read(Scanner input) {
        int states = input.nextInt(), edges = input.nextInt();
        var a = new NestedWordAutomaton<Integer, String>(SERVICES,
            new VpAlphabet<>(Set.of(2), Set.of(0), Set.of(1)), FACTORY);
        for (int q = 0; q < states; ++q)
            a.addState(input.nextInt() != 0, input.nextInt() != 0, "q" + q);
        for (int i = 0; i < edges; ++i) {
            String from = "q" + input.nextInt();
            int letter = input.nextInt();
            String to = "q" + input.nextInt();
            int hierarchy = input.nextInt();
            if (letter == 0) a.addCallTransition(from, letter, to);
            else if (letter == 2) a.addInternalTransition(from, letter, to);
            else a.addReturnTransition(from, hierarchy < 0 ? a.getEmptyStackState() : "q" + hierarchy,
                                       letter, to);
        }
        return a;
    }

    private static NestedWord<Integer> word(Scanner input) {
        int size = input.nextInt();
        Integer[] letters = new Integer[size];
        int[] matching = new int[size];
        Arrays.fill(matching, NestedWord.INTERNAL_POSITION);
        var calls = new ArrayDeque<Integer>();
        for (int i = 0; i < size; ++i) {
            letters[i] = input.nextInt();
            if (letters[i] == 0) {
                calls.push(i);
                matching[i] = NestedWord.PLUS_INFINITY;
            } else if (letters[i] == 1) {
                if (calls.isEmpty()) {
                    throw new IllegalArgumentException("Ultimate oracle excludes pending returns");
                }
                int call = calls.pop();
                matching[call] = i;
                matching[i] = call;
            }
        }
        return new NestedWord<>(letters, matching);
    }

    private static int bit(boolean value) { return value ? 1 : 0; }

    private static Integer[] rawWord(Scanner input) {
        Integer[] result = new Integer[input.nextInt()];
        for (int i = 0; i < result.length; ++i) result[i] = input.nextInt();
        return result;
    }

    private static boolean periodic(INwaOutgoingLetterAndTransitionProvider<Integer, String> a,
                                    Integer[] stem, Integer[] loop) throws Exception {
        if (loop.length == 0) throw new IllegalArgumentException("empty omega loop");
        var monitor = new NestedWordAutomaton<Integer, String>(SERVICES,
            new VpAlphabet<>(Set.of(2), Set.of(0), Set.of(1)), FACTORY);
        int size = stem.length + loop.length;
        for (int q = 0; q < size; ++q) monitor.addState(q == 0, q >= stem.length, "m" + q);
        for (int q = 0; q < size; ++q) {
            int letter = q < stem.length ? stem[q] : loop[q - stem.length];
            String from = "m" + q, to = "m" + (q + 1 < size ? q + 1 : stem.length);
            if (letter == 0) monitor.addCallTransition(from, letter, to);
            else if (letter == 2) monitor.addInternalTransition(from, letter, to);
            else {
                for (int h = 0; h < size; ++h) monitor.addReturnTransition(from, "m" + h, letter, to);
            }
        }
        var product = new Intersect<>(SERVICES, FACTORY, a, monitor).getResult();
        return !new BuchiIsEmpty<>(SERVICES, product).getResult();
    }

    public static void main(String[] args) throws Exception {
        try (var input = new Scanner(new BufferedInputStream(System.in))) {
            int cases = input.nextInt();
            for (int example = 0; example < cases; ++example) {
                var a = read(input);
                var b = read(input);
                var da = new Determinize<>(SERVICES, FACTORY, a).getResult();
                var db = new Determinize<>(SERVICES, FACTORY, b).getResult();
                var diff = new Difference<>(SERVICES, FACTORY, a, b).getResult();
                var comp = new Complement<>(SERVICES, FACTORY, a).getResult();
                var product = new Intersect<>(SERVICES, FACTORY, a, b).getResult();
                var pruned = new RemoveDeadEnds<>(SERVICES, a).getResult();
                INwaOutgoingLetterAndTransitionProvider<Integer, String> direct = a, delayed = a;
                if (a.size() != 0) {
                    var reachable = new NestedWordAutomatonReachableStates<>(SERVICES, a);
                    direct = new ReduceNwaDirectSimulationB<>(SERVICES, FACTORY, reachable).getResult();
                    delayed = new ReduceNwaDelayedSimulationB<>(SERVICES, FACTORY, reachable).getResult();
                }
                System.out.println("case " + example + " " +
                    bit(new IsEmpty<>(SERVICES, a).getResult()) + " " +
                    bit(new IsEmpty<>(SERVICES, b).getResult()) + " " +
                    bit(new IsEmpty<>(SERVICES, diff).getResult()) + " " +
                    bit(new BuchiIsEmpty<>(SERVICES, a).getResult()) + " " +
                    bit(new BuchiIsEmpty<>(SERVICES, b).getResult()));
                @SuppressWarnings("unchecked")
                INwaOutgoingLetterAndTransitionProvider<Integer, String>[] operands =
                    new INwaOutgoingLetterAndTransitionProvider[]{a, b, da, db, diff, comp, product, pruned, direct, direct};
                int queries = input.nextInt();
                for (int i = 0; i < queries; ++i) {
                    var w = word(input);
                    var line = new StringBuilder();
                    for (var operand : operands)
                        line.append(bit(new Accepts<>(SERVICES, operand, w).getResult()));
                    System.out.println(line);
                }
                int omegaQueries = input.nextInt();
                for (int i = 0; i < omegaQueries; ++i) {
                    var stem = rawWord(input);
                    var loop = rawWord(input);
                    System.out.println("omega " + bit(periodic(a, stem, loop)) +
                        bit(periodic(direct, stem, loop)) + bit(periodic(delayed, stem, loop)) +
                        bit(periodic(direct, stem, loop)) + bit(periodic(delayed, stem, loop)));
                }
            }
            if (input.hasNext()) throw new IllegalArgumentException("trailing corpus input");
        }
    }
}
