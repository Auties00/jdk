import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

// JDK-8365196 scrub6: where C2 puts the scrubs. Run on a fastdebug VM with the scrub forced
// (-XX:ExceptionXStateLength=1920) and -XX:CompileCommand=print, and count "trap scrub" per method.
public class ScrubPlacement {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;

    static final class Node {
        Node next;
        double v;
    }

    static double counted(Node head) {
        DoubleVector acc = DoubleVector.zero(S);
        Node p = head;
        for (int i = 0; i < 1024; i++) {
            acc = acc.add(p.v);
            p = p.next;
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    static double uncounted(Node head) {
        DoubleVector acc = DoubleVector.zero(S);
        for (Node p = head; p != null; p = p.next) {
            acc = acc.add(p.v);
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    static double callee(double x) {
        return x + 1;
    }

    static double withCall(Node head) {
        DoubleVector acc = DoubleVector.zero(S);
        Node p = head;
        for (int i = 0; i < 1024; i++) {
            acc = acc.add(callee(p.v));
            p = p.next;
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    static double scalar(Node head) {
        double acc = 0;
        for (Node p = head; p != null; p = p.next) {
            acc += p.v;
        }
        return acc;
    }

    public static void main(String[] args) {
        Node[] nodes = new Node[1024];
        for (int i = 0; i < nodes.length; i++) {
            nodes[i] = new Node();
            nodes[i].v = i;
        }
        for (int i = 0; i < nodes.length - 1; i++) {
            nodes[i].next = nodes[i + 1];
        }
        Node ring = nodes[0];
        Node[] ringNodes = new Node[1024];
        for (int i = 0; i < 1024; i++) {
            ringNodes[i] = new Node();
            ringNodes[i].v = i;
        }
        for (int i = 0; i < 1024; i++) {
            ringNodes[i].next = ringNodes[(i + 1) & 1023];
        }
        double sum = 0;
        for (int r = 0; r < 20000; r++) {
            sum += counted(ringNodes[0]) + uncounted(ring) + withCall(ringNodes[0]) + scalar(ring);
        }
        System.out.println(sum);
    }
}
