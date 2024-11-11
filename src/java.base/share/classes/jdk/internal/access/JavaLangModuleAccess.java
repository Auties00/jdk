/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.internal.access;

import java.io.PrintStream;
import java.lang.module.Configuration;
import java.lang.module.ModuleDescriptor;
import java.lang.module.ModuleDescriptor.*;
import jdk.internal.module.ModuleDirectiveAnnotation;
import java.lang.module.ModuleFinder;
import java.util.*;

/**
 * Provides access to non-public methods in java.lang.module.
 */

public interface JavaLangModuleAccess {

    /**
     * Creates a builder for building a module with the given module name.
     *
     * @param strict
     *        Indicates whether module names are checked or not
     */
    ModuleDescriptor.Builder newModuleBuilder(String mn,
                                              boolean strict,
                                              Set<ModuleDescriptor.Modifier> ms);

    /**
     * Returns a snapshot of the packages in the module.
     */
    Set<String> packages(ModuleDescriptor.Builder builder);

    /**
     * Adds a dependence on a module with the given (possibly un-parsable)
     * version string.
     */
    void requires(ModuleDescriptor.Builder builder,
                  Set<Requires.Modifier> ms,
                  String mn,
                  String rawCompiledVersion,
                  List<? extends ModuleDirectiveAnnotation> annotations);

    /**
     * Returns a {@code ModuleDescriptor.Requires} of the given modifiers
     * and module name.
     */
    Requires newRequires(Set<Requires.Modifier> ms, String mn, Version v);

    /**
     * Returns an unqualified {@code ModuleDescriptor.Exports}
     * of the given modifiers and package name source.
     */
    Exports newExports(Set<Exports.Modifier> ms,
                       String source);

    /**
     * Returns a qualified {@code ModuleDescriptor.Exports}
     * of the given modifiers, package name source and targets.
     */
    Exports newExports(Set<Exports.Modifier> ms,
                       String source,
                       Set<String> targets);

    /**
     * Returns an unqualified {@code ModuleDescriptor.Opens}
     * of the given modifiers and package name source.
     */
    Opens newOpens(Set<Opens.Modifier> ms, String source);

    /**
     * Returns a qualified {@code ModuleDescriptor.Opens}
     * of the given modifiers, package name source and targets.
     */
    Opens newOpens(Set<Opens.Modifier> ms, String source, Set<String> targets);

    /**
     * Returns a qualified {@code ModuleDescriptor.Uses } of the given service.
     */
    Uses newUses(String serviceName);

    /**
     * Returns a {@code ModuleDescriptor.Provides}
     * of the given service name and providers.
     */
    Provides newProvides(String service, List<String> providers);

    /**
     * Returns a new {@code ModuleDescriptor} instance.
     */
    ModuleDescriptor newModuleDescriptor(String name,
                                         Version version,
                                         Set<ModuleDescriptor.Modifier> ms,
                                         Set<Requires> requires,
                                         Set<Exports> exports,
                                         Set<Opens> opens,
                                         Set<Uses> uses,
                                         Set<Provides> provides,
                                         Set<String> packages,
                                         String mainClass,
                                         int hashCode);

    /**
     * Resolves a collection of root modules, with service binding
     * and the empty configuration as the parent.
     */
    Configuration resolveAndBind(ModuleFinder finder,
                                 Collection<String> roots,
                                 PrintStream traceOutput);

    /**
     * Creates a configuration from a pre-generated readability graph.
     */
    Configuration newConfiguration(ModuleFinder finder,
                                   Map<String, Set<String>> graph);

    Exports newExports(Set<Exports.Modifier> modifiers, String s, Set<String> targets, List<ModuleDirectiveAnnotation> annotations);

    Opens newOpens(Set<Opens.Modifier> modifiers, String s, Set<String> targets, List<ModuleDirectiveAnnotation> annotations);

    Uses newUses(String serviceName, List<ModuleDirectiveAnnotation> annotations);

    Provides newProvides(String s, List<String> providers, List<ModuleDirectiveAnnotation> annotations);
}
