#ifndef SORBET_SYMBOLS_H
#define SORBET_SYMBOLS_H

#include "common/common.h"
#include "core/Loc.h"
#include "core/Names.h"
#include "core/SymbolRef.h"
#include "core/Types.h"
#include <memory>
#include <tuple>
#include <vector>

namespace sorbet::core {
class Symbol;
class GlobalState;
struct GlobalStateHash;
class Type;
class MutableContext;
class Context;
class TypeAndOrigins;
class SendAndBlockLink;
struct DispatchArgs;
struct CallLocs;

namespace serialize {
class SerializerImpl;
}
class IntrinsicMethod {
public:
    virtual TypePtr apply(Context ctx, DispatchArgs args, const Type *thisType) const = 0;
};

enum class Variance { CoVariant = 1, ContraVariant = -1, Invariant = 0 };

class Symbol final {
public:
    Symbol(const Symbol &) = delete;
    Symbol() = default;
    Symbol(Symbol &&) noexcept = default;

    class Flags {
    public:
        static constexpr u4 NONE = 0;

        // We're packing three different kinds of flags into separate ranges with the u4's below:
        //
        // 0x0000'0000
        //   ├▶    ◀┤└─ Applies to all types of symbol
        //   │      │
        //   │      └─ For our current symbol type, what flags does it have?
        //   │         (New flags grow up towards MSB)
        //   │
        //   └─ What type of symbol is this?
        //      (New flags grow down towards LSB)
        //

        // --- What type of symbol is this? ---
        static constexpr u4 CLASS = 0x8000'0000;
        static constexpr u4 METHOD = 0x4000'0000;
        static constexpr u4 FIELD = 0x2000'0000;
        static constexpr u4 STATIC_FIELD = 0x1000'0000;
        static constexpr u4 TYPE_ARGUMENT = 0x0800'0000;
        static constexpr u4 TYPE_MEMBER = 0x0400'0000;

        // --- Applies to all types of Symbols ---

        // Synthesized by C++ code in a DSL pass
        static constexpr u4 DSL_SYNTHESIZED = 0x0000'0001;

        // --- For our current symbol type, what flags does it have?

        // Class flags
        static constexpr u4 CLASS_CLASS = 0x0000'0010;
        static constexpr u4 CLASS_MODULE = 0x0000'0020;
        static constexpr u4 CLASS_ABSTRACT = 0x0000'0040;
        static constexpr u4 CLASS_INTERFACE = 0x0000'0080;
        static constexpr u4 CLASS_LINEARIZATION_COMPUTED = 0x0000'0100;

        // Method flags
        static constexpr u4 METHOD_PROTECTED = 0x0000'0010;
        static constexpr u4 METHOD_PRIVATE = 0x0000'0020;
        static constexpr u4 METHOD_OVERLOADED = 0x0000'0040;
        static constexpr u4 METHOD_ABSTRACT = 0x0000'0080;
        static constexpr u4 METHOD_GENERIC = 0x0000'0100;
        static constexpr u4 METHOD_GENERATED_SIG = 0x0000'0200;
        static constexpr u4 METHOD_OVERRIDABLE = 0x0000'0400;
        static constexpr u4 METHOD_FINAL = 0x0000'0800;
        static constexpr u4 METHOD_OVERRIDE = 0x0000'1000;
        static constexpr u4 METHOD_IMPLEMENTATION = 0x0000'2000;
        static constexpr u4 METHOD_INCOMPATIBLE_OVERRIDE = 0x0000'4000;

        // Type flags
        static constexpr u4 TYPE_COVARIANT = 0x0000'0010;
        static constexpr u4 TYPE_INVARIANT = 0x0000'0020;
        static constexpr u4 TYPE_CONTRAVARIANT = 0x0000'0040;
        static constexpr u4 TYPE_FIXED = 0x0000'0080;

        // Static Field flags
        static constexpr u4 STATIC_FIELD_TYPE_ALIAS = 0x0000'0010;
    };

    Loc loc() const;
    const InlinedVector<Loc, 2> &locs() const;
    void addLoc(const core::GlobalState &gs, core::Loc loc);

    u4 hash(const GlobalState &gs) const;
    u4 methodShapeHash(const GlobalState &gs) const;

    std::vector<TypePtr> selfTypeArgs(const GlobalState &gs) const;

    // selfType and externalType return the type of an instance of this Symbol
    // (which must be isClass()), if instantiated without specific type
    // parameters, as seen from inside or outside of the class, respectively.
    TypePtr selfType(const GlobalState &gs) const;
    TypePtr externalType(const GlobalState &gs) const;

    inline InlinedVector<SymbolRef, 4> &mixins() {
        ENFORCE(isClass());
        return mixins_;
    }

    inline const InlinedVector<SymbolRef, 4> &mixins() const {
        ENFORCE(isClass());
        return mixins_;
    }

    inline InlinedVector<SymbolRef, 4> &typeMembers() {
        ENFORCE(isClass());
        return typeParams;
    }

    inline const InlinedVector<SymbolRef, 4> &typeMembers() const {
        ENFORCE(isClass());
        return typeParams;
    }

    // Return the number of type parameters that must be passed to instantiate
    // this generic type. May differ from typeMembers.size() if some type
    // members have fixed values.
    int typeArity(const GlobalState &gs) const;

    inline InlinedVector<SymbolRef, 4> &typeArguments() {
        ENFORCE(isMethod());
        return typeParams;
    }

    inline const InlinedVector<SymbolRef, 4> &typeArguments() const {
        ENFORCE(isMethod());
        return typeParams;
    }

    bool derivesFrom(const GlobalState &gs, SymbolRef sym) const;

    // TODO(dmitry) perf: most calls to this method could be eliminated as part of perf work.
    SymbolRef ref(const GlobalState &gs) const;

    inline bool isClass() const {
        return (flags & Symbol::Flags::CLASS) != 0;
    }

    bool isSingletonClass(const GlobalState &gs) const;

    inline bool isStaticField() const {
        return (flags & Symbol::Flags::STATIC_FIELD) != 0;
    }

    inline bool isField() const {
        return (flags & Symbol::Flags::FIELD) != 0;
    }

    inline bool isMethod() const {
        return (flags & Symbol::Flags::METHOD) != 0;
    }

    inline bool isTypeMember() const {
        return (flags & Symbol::Flags::TYPE_MEMBER) != 0;
    }

    inline bool isTypeArgument() const {
        return (flags & Symbol::Flags::TYPE_ARGUMENT) != 0;
    }

    inline bool isOverloaded() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_OVERLOADED) != 0;
    }

    inline bool isAbstract() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_ABSTRACT) != 0;
    }

    inline bool isImplementation() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_IMPLEMENTATION) != 0;
    }

    inline bool isIncompatibleOverride() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_INCOMPATIBLE_OVERRIDE) != 0;
    }

    inline bool isGenericMethod() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_GENERIC) != 0;
    }

    inline bool isOverridable() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_OVERRIDABLE) != 0;
    }

    inline bool isOverride() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_OVERRIDE) != 0;
    }

    inline bool hasGeneratedSig() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_GENERATED_SIG) != 0;
    }

    inline bool isCovariant() const {
        ENFORCE(isTypeArgument() || isTypeMember());
        return (flags & Symbol::Flags::TYPE_COVARIANT) != 0;
    }

    inline bool isInvariant() const {
        ENFORCE(isTypeArgument() || isTypeMember());
        return (flags & Symbol::Flags::TYPE_INVARIANT) != 0;
    }

    inline bool isContravariant() const {
        ENFORCE(isTypeArgument() || isTypeMember());
        return (flags & Symbol::Flags::TYPE_CONTRAVARIANT) != 0;
    }

    inline bool isFixed() const {
        ENFORCE(isTypeArgument() || isTypeMember());
        return (flags & Symbol::Flags::TYPE_FIXED) != 0;
    }

    Variance variance() const {
        if (isInvariant())
            return Variance::Invariant;
        if (isCovariant())
            return Variance::CoVariant;
        if (isContravariant())
            return Variance::ContraVariant;
        Exception::raise("Should not happen");
    }

    inline bool isPublic() const {
        ENFORCE(isMethod());
        return !isProtected() && !isPrivate();
    }

    inline bool isProtected() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_PROTECTED) != 0;
    }

    inline bool isPrivate() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_PRIVATE) != 0;
    }

    inline bool isClassModule() const {
        ENFORCE(isClass());
        if (flags & Symbol::Flags::CLASS_MODULE)
            return true;
        if (flags & Symbol::Flags::CLASS_CLASS)
            return false;
        Exception::raise("Should never happen");
    }

    inline bool isClassModuleSet() const {
        ENFORCE(isClass());
        return flags & (Symbol::Flags::CLASS_MODULE | Symbol::Flags::CLASS_CLASS);
    }

    inline bool isClassClass() const {
        return !isClassModule();
    }

    inline bool isClassAbstract() const {
        ENFORCE(isClass());
        return (flags & Symbol::Flags::CLASS_ABSTRACT) != 0;
    }

    inline bool isClassInterface() const {
        ENFORCE(isClass());
        return (flags & Symbol::Flags::CLASS_INTERFACE) != 0;
    }

    inline bool isClassLinearizationComputed() const {
        ENFORCE(isClass());
        return (flags & Symbol::Flags::CLASS_LINEARIZATION_COMPUTED) != 0;
    }

    inline void setClass() {
        ENFORCE(!isStaticField() && !isField() && !isMethod() && !isTypeArgument() && !isTypeMember());
        flags = flags | Symbol::Flags::CLASS;
    }

    inline void setStaticField() {
        ENFORCE(!isClass() && !isField() && !isMethod() && !isTypeArgument() && !isTypeMember());
        flags = flags | Symbol::Flags::STATIC_FIELD;
    }

    inline void setField() {
        ENFORCE(!isClass() && !isStaticField() && !isMethod() && !isTypeArgument() && !isTypeMember());
        flags = flags | Symbol::Flags::FIELD;
    }

    inline void setMethod() {
        ENFORCE(!isClass() && !isStaticField() && !isField() && !isTypeArgument() && !isTypeMember());
        flags = flags | Symbol::Flags::METHOD;
    }

    inline void setTypeArgument() {
        ENFORCE(!isClass() && !isStaticField() && !isField() && !isMethod() && !isTypeMember());
        flags = flags | Symbol::Flags::TYPE_ARGUMENT;
    }

    inline void setTypeMember() {
        ENFORCE(!isClass() && !isStaticField() && !isField() && !isMethod() && !isTypeArgument());
        flags = flags | Symbol::Flags::TYPE_MEMBER;
    }

    inline void setIsModule(bool isModule) {
        ENFORCE(isClass());
        if (isModule) {
            ENFORCE((flags & Symbol::Flags::CLASS_CLASS) == 0);
            flags = flags | Symbol::Flags::CLASS_MODULE;
        } else {
            ENFORCE((flags & Symbol::Flags::CLASS_MODULE) == 0);
            flags = flags | Symbol::Flags::CLASS_CLASS;
        }
    }

    inline void setCovariant() {
        ENFORCE(isTypeArgument() || isTypeMember());
        ENFORCE(!isContravariant() && !isInvariant());
        flags |= Symbol::Flags::TYPE_COVARIANT;
    }

    inline void setContravariant() {
        ENFORCE(isTypeArgument() || isTypeMember());
        ENFORCE(!isCovariant() && !isInvariant());
        flags |= Symbol::Flags::TYPE_CONTRAVARIANT;
    }

    inline void setInvariant() {
        ENFORCE(isTypeArgument() || isTypeMember());
        ENFORCE(!isCovariant() && !isContravariant());
        flags |= Symbol::Flags::TYPE_INVARIANT;
    }

    inline void setFixed() {
        ENFORCE(isTypeArgument() || isTypeMember());
        flags |= Symbol::Flags::TYPE_FIXED;
    }

    inline void setOverloaded() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_OVERLOADED;
    }

    inline void setAbstract() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_ABSTRACT;
    }

    inline void setImplementation() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_IMPLEMENTATION;
    }

    inline void setIncompatibleOverride() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_INCOMPATIBLE_OVERRIDE;
    }

    inline void setGenericMethod() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_GENERIC;
    }

    inline void setOverridable() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_OVERRIDABLE;
    }

    inline void setFinalMethod() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_FINAL;
    }

    inline void setOverride() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_OVERRIDE;
    }

    inline bool isFinalMethod() const {
        ENFORCE(isMethod());
        return (flags & Symbol::Flags::METHOD_FINAL) != 0;
    }

    inline void setHasGeneratedSig() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_GENERATED_SIG;
    }

    inline void unsetHasGeneratedSig() {
        ENFORCE(isMethod());
        flags &= ~Symbol::Flags::METHOD_GENERATED_SIG;
    }

    inline void setPublic() {
        ENFORCE(isMethod());
        flags &= ~Symbol::Flags::METHOD_PRIVATE;
        flags &= ~Symbol::Flags::METHOD_PROTECTED;
    }

    inline void setProtected() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_PROTECTED;
    }

    inline void setPrivate() {
        ENFORCE(isMethod());
        flags |= Symbol::Flags::METHOD_PRIVATE;
    }

    inline void setClassAbstract() {
        ENFORCE(isClass());
        flags |= Symbol::Flags::CLASS_ABSTRACT;
    }

    inline void setClassInterface() {
        ENFORCE(isClass());
        flags |= Symbol::Flags::CLASS_INTERFACE;
    }

    inline void setClassLinearizationComputed() {
        ENFORCE(isClass());
        flags |= Symbol::Flags::CLASS_LINEARIZATION_COMPUTED;
    }

    inline void setTypeAlias() {
        ENFORCE(isStaticField());
        flags |= Symbol::Flags::STATIC_FIELD_TYPE_ALIAS;
    }
    inline bool isTypeAlias() const {
        // We should only be able to set the type alias bit on static fields.
        // But it's rather unweidly to ask "isStaticField() && isTypeAlias()" just to satisfy the ENFORCE.
        // To make things nicer, we relax the ENFORCE here to also allow asking whether "some constant" is a type alias.
        ENFORCE(isClass() || isStaticField() || isTypeMember());
        return isStaticField() && (flags & Symbol::Flags::STATIC_FIELD_TYPE_ALIAS) != 0;
    }

    inline void setDSLSynthesized() {
        flags |= Symbol::Flags::DSL_SYNTHESIZED;
    }
    inline bool isDSLSynthesized() const {
        return (flags & Symbol::Flags::DSL_SYNTHESIZED) != 0;
    }

    SymbolRef findMember(const GlobalState &gs, NameRef name) const;
    SymbolRef findMemberNoDealias(const GlobalState &gs, NameRef name) const;
    SymbolRef findMemberTransitive(const GlobalState &gs, NameRef name) const;
    SymbolRef findConcreteMethodTransitive(const GlobalState &gs, NameRef name) const;

    /* transitively finds a member with the most similar name */

    struct FuzzySearchResult {
        SymbolRef symbol;
        NameRef name;
        int distance;
    };

    std::vector<FuzzySearchResult> findMemberFuzzyMatch(const GlobalState &gs, NameRef name, int betterThan = -1) const;

    std::string toStringFullName(const GlobalState &gs) const;
    std::string showFullName(const GlobalState &gs) const;

    // Not printed when showing name table
    bool isHiddenFromPrinting(const GlobalState &gs) const;

    std::string showRaw(const GlobalState &gs) const {
        bool showFull = false;
        bool showRaw = true;
        return toStringWithOptions(gs, 0, showFull, showRaw);
    }
    std::string toString(const GlobalState &gs) const {
        bool showFull = false;
        bool showRaw = false;
        return toStringWithOptions(gs, 0, showFull, showRaw);
    }
    std::string toJSON(const GlobalState &gs, int tabs = 0, bool showFull = false) const;
    // Renders the full name of this Symbol in a form suitable for user display.
    std::string show(const GlobalState &gs) const;

    // Returns the singleton class for this class, lazily instantiating it if it
    // doesn't exist.
    SymbolRef singletonClass(GlobalState &gs);

    // Returns the singleton class or noSymbol
    SymbolRef lookupSingletonClass(const GlobalState &gs) const;

    // Returns attached class or noSymbol if it does not exist
    SymbolRef attachedClass(const GlobalState &gs) const;

    SymbolRef topAttachedClass(const GlobalState &gs) const;

    SymbolRef dealias(const GlobalState &gs, int depthLimit = 42) const;

    bool ignoreInHashing(const GlobalState &gs) const;

    SymbolRef owner;
    SymbolRef superClassOrRebind; // method arugments store rebind here

    inline SymbolRef superClass() const {
        ENFORCE(isClass());
        return superClassOrRebind;
    }

    inline void setSuperClass(SymbolRef claz) {
        ENFORCE(isClass());
        superClassOrRebind = claz;
    }

    inline void setReBind(SymbolRef rebind) {
        ENFORCE(isMethod());
        superClassOrRebind = rebind;
    }

    SymbolRef rebind() const {
        ENFORCE(isMethod());
        return superClassOrRebind;
    }

    u4 flags = Flags::NONE;
    u4 uniqueCounter = 1; // used as a counter inside the namer
    NameRef name;         // todo: move out? it should not matter but it's important for name resolution
    TypePtr resultType;

    UnorderedMap<NameRef, SymbolRef> members_;
    std::vector<ArgInfo> arguments_;

    UnorderedMap<NameRef, SymbolRef> &members() {
        return members_;
    };
    const UnorderedMap<NameRef, SymbolRef> &members() const {
        return members_;
    };

    std::vector<ArgInfo> &arguments() {
        return arguments_;
    }

    const std::vector<ArgInfo> &arguments() const {
        return arguments_;
    }

    std::vector<std::pair<NameRef, SymbolRef>> membersStableOrderSlow(const GlobalState &gs) const;

    Symbol deepCopy(const GlobalState &to, bool keepGsId = false) const;
    void sanityCheck(const GlobalState &gs) const;
    SymbolRef enclosingMethod(const GlobalState &gs) const;

    SymbolRef enclosingClass(const GlobalState &gs) const;

    // All `IntrinsicMethod`s in sorbet should be statically-allocated, which is
    // why raw pointers are safe.
    const IntrinsicMethod *intrinsic = nullptr;

private:
    friend class serialize::SerializerImpl;
    friend class GlobalState;

    std::string toStringWithOptions(const GlobalState &gs, int tabs = 0, bool showFull = false,
                                    bool showRaw = false) const;

    FuzzySearchResult findMemberFuzzyMatchUTF8(const GlobalState &gs, NameRef name, int betterThan = -1) const;
    std::vector<FuzzySearchResult> findMemberFuzzyMatchConstant(const GlobalState &gs, NameRef name,
                                                                int betterThan = -1) const;

    /*
     * mixins and superclasses: `superClass` is *not* included in the
     *   `argumentsOrMixins` list. `superClass` may not exist even if
     *   `isClass()`, which implies that this symbol is either a module or one
     *   of our magic synthetic classes. During parsing+naming, `superClass ==
     *   todo()` iff every definition we've seen for this class has had an
     *   implicit superclass (`class Foo` with no `< Parent`); Once we hit
     *   Resolver::finalize(), these will be rewritten to `Object()`.
     */
    InlinedVector<SymbolRef, 4> mixins_;

    /** For Class or module - ordered type members of the class,
     * for method - ordered type generic type arguments of the class
     */
    InlinedVector<SymbolRef, 4> typeParams;
    InlinedVector<Loc, 2> locs_;

    SymbolRef findMemberTransitiveInternal(const GlobalState &gs, NameRef name, u4 mask, u4 flags,
                                           int maxDepth = 100) const;
};
// CheckSize(Symbol, 144, 8); // This is under too much churn to be worth checking

} // namespace sorbet::core
#endif // SORBET_SYMBOLS_H
