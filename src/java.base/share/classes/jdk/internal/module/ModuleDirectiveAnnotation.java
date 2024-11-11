package jdk.internal.module;

import sun.reflect.annotation.AnnotationParser;
import sun.reflect.generics.factory.CoreReflectionFactory;
import sun.reflect.generics.factory.GenericsFactory;
import sun.reflect.generics.parser.SignatureParser;
import sun.reflect.generics.scope.ClassScope;
import sun.reflect.generics.tree.TypeSignature;
import sun.reflect.generics.visitor.Reifier;

import java.lang.annotation.Annotation;
import java.lang.reflect.Type;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A module's directive annotation.
 *
 * <p>
 * When the module system is first booted, the Proxy system that {@link AnnotationParser} uses to initialize annotations is not enabled.
 * For this reason it's necessary to define a raw and concrete implementations of a module's directive annotation.
 * When the annotation data is queried, it's assumed that the module system will have already finished booting, so creating the Proxy should not be a problem.
 * </p>
 */
public sealed abstract class ModuleDirectiveAnnotation {
    public static ModuleDirectiveAnnotation of(String typeSignature, Map<String, AttributeDescriptor> values) {
        return new Descriptor(typeSignature, values);
    }

    public static ModuleDirectiveAnnotation of(Annotation annotation) {
        return new Parsed(annotation);
    }

    public static List<? extends ModuleDirectiveAnnotation> of(Collection<? extends Annotation> annotations) {
        return annotations.stream()
                .map(Parsed::new)
                .toList();
    }

    public abstract Annotation toAnnotation(Class<?> caller);

    public sealed abstract static class AttributeDescriptor {
        public static AttributeDescriptor ofPrimitive(byte entry) {
            return new ByteConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(short entry) {
            return new ShortConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(int entry) {
            return new IntConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(long entry) {
            return new LongConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(float entry) {
            return new FloatConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(double entry) {
            return new DoubleConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(boolean entry) {
            return new BooleanConstant(entry);
        }

        public static AttributeDescriptor ofPrimitive(char entry) {
            return new CharConstant(entry);
        }

        public static AttributeDescriptor ofString(String entry) {
            return new StringLiteral(entry);
        }

        public static AttributeDescriptor ofClass(String entry) {
            return new ClassLiteral(entry);
        }

        public static AttributeDescriptor ofEnum(String typeSignature, String enumConstant) {
            return new EnumConstant(typeSignature, enumConstant);
        }

        public static AttributeDescriptor ofAnnotation(ModuleDirectiveAnnotation annotation) {
            return new AnnotationValue(annotation);
        }

        public static AttributeDescriptor ofArray(AttributeDescriptor[] array) {
            return new ArrayValue(array);
        }

        public abstract Object toAnnotationValue(Class<?> caller);

        private static final class ByteConstant extends AttributeDescriptor {
            private final byte value;
            private ByteConstant(byte value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class ShortConstant extends AttributeDescriptor {
            private final short value;
            private ShortConstant(short value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class IntConstant extends AttributeDescriptor {
            private final int value;
            private IntConstant(int value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class LongConstant extends AttributeDescriptor {
            private final long value;
            private LongConstant(long value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class FloatConstant extends AttributeDescriptor {
            private final float value;
            private FloatConstant(float value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class DoubleConstant extends AttributeDescriptor {
            private final double value;
            private DoubleConstant(double value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class BooleanConstant extends AttributeDescriptor {
            private final boolean value;
            private BooleanConstant(boolean value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class CharConstant extends AttributeDescriptor {
            private final char value;
            private CharConstant(char value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class StringLiteral extends AttributeDescriptor {
            private final String value;
            private StringLiteral(String value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value;
            }
        }

        private static final class ClassLiteral extends AttributeDescriptor {
            private final String typeSignature;
            private ClassLiteral(String typeSignature) {
                this.typeSignature = typeSignature;
            }


            @Override
            public Object toAnnotationValue(Class<?> caller) {
                SignatureParser parser = SignatureParser.make();
                TypeSignature typeSig = parser.parseTypeSig(typeSignature);
                GenericsFactory factory = CoreReflectionFactory.make(caller, ClassScope.make(caller));
                Reifier reify = Reifier.make(factory);
                typeSig.accept(reify);
                return reify.getResult();
            }
        }

        private static final class AnnotationValue extends AttributeDescriptor {
            private final ModuleDirectiveAnnotation value;
            private AnnotationValue(ModuleDirectiveAnnotation value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                return value.toAnnotation(caller);
            }
        }

        private static final class ArrayValue extends AttributeDescriptor {
            private final AttributeDescriptor[] value;
            private ArrayValue(AttributeDescriptor[] value) {
                this.value = value;
            }

            @Override
            public Object toAnnotationValue(Class<?> caller) {
                Object[] result = new Object[value.length];
                for (int i = 0; i < value.length; i++) {
                    result[i] = value[i].toAnnotationValue(caller);
                }
                return result;
            }
        }

        private static final class EnumConstant extends AttributeDescriptor {
            private final String typeSignature;
            private final String constantName;
            private EnumConstant(String typeSignature, String constantName) {
                this.typeSignature = typeSignature;
                this.constantName = constantName;
            }

            @Override
            @SuppressWarnings({"rawtypes", "unchecked"})
            public Object toAnnotationValue(Class<?> caller) {
                SignatureParser parser = SignatureParser.make();
                TypeSignature typeSig = parser.parseTypeSig(typeSignature);
                GenericsFactory factory = CoreReflectionFactory.make(caller, ClassScope.make(caller));
                Reifier reify = Reifier.make(factory);
                typeSig.accept(reify);
                Type result = reify.getResult();
                Class<? extends Enum> annotationType = (Class<? extends Enum>) result;
                return Enum.valueOf(annotationType, constantName);
            }
        }
    }

    private static final class Descriptor extends ModuleDirectiveAnnotation {
        private final String typeSignature;
        private final Map<String, ? extends AttributeDescriptor> values;
        private Descriptor(String typeSignature, Map<String, ? extends AttributeDescriptor> values) {
            this.typeSignature = typeSignature;
            this.values = values;
        }

        @SuppressWarnings("unchecked")
        @Override
        public Annotation toAnnotation(Class<?> caller) {
            SignatureParser parser = SignatureParser.make();
            TypeSignature typeSig = parser.parseTypeSig(typeSignature);
            GenericsFactory factory = CoreReflectionFactory.make(caller, ClassScope.make(caller));
            Reifier reify = Reifier.make(factory);
            typeSig.accept(reify);
            Type result = reify.getResult();
            Class<? extends Annotation> annotationType = (Class<? extends Annotation>) result;
            Map<String, Object> parsedValues = new LinkedHashMap<>();
            for (Map.Entry<String, ? extends AttributeDescriptor> entry : values.entrySet()) {
                String key = entry.getKey();
                Object value = entry.getValue().toAnnotationValue(caller);
                parsedValues.put(key, value);
            }
            return AnnotationParser.annotationForMap(annotationType, parsedValues);
        }
    }

    private static final class Parsed extends ModuleDirectiveAnnotation {
        private final Annotation value;
        private Parsed(Annotation value) {
            this.value = value;
        }

        @Override
        public Annotation toAnnotation(Class<?> caller) {
            return value;
        }
    }
}
