#include "resolver/type_syntax.h"
#include "common/typecase.h"
#include "core/Names.h"
#include "core/Symbols.h"
#include "core/core.h"
#include "core/errors/resolver.h"

using namespace std;

namespace sorbet::resolver {

core::TypePtr getResultLiteral(core::Context ctx, unique_ptr<ast::Expression> &expr) {
    core::TypePtr result;
    typecase(
        expr.get(), [&](ast::Literal *lit) { result = lit->value; },
        [&](ast::Expression *expr) {
            if (auto e = ctx.state.beginError(expr->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                e.setHeader("Unsupported type literal");
            }
            result = core::Types::untypedUntracked();
        });
    ENFORCE(result.get() != nullptr);
    result->sanityCheck(ctx);
    return result;
}

bool isTProc(core::Context ctx, ast::Send *send) {
    while (send != nullptr) {
        if (send->fun == core::Names::proc()) {
            auto recv = send->recv.get();
            if (auto *rcv = ast::cast_tree<ast::ConstantLit>(recv)) {
                return rcv->symbol == core::Symbols::T();
            }
        }
        send = ast::cast_tree<ast::Send>(send->recv.get());
    }
    return false;
}

bool TypeSyntax::isSig(core::Context ctx, ast::Send *send) {
    if (send->fun != core::Names::sig()) {
        return false;
    }
    if (send->block.get() == nullptr) {
        return false;
    }
    if (!send->args.empty()) {
        return false;
    }

    // self.sig
    if (send->recv->isSelfReference()) {
        return true;
    }

    // Sorbet.sig
    auto recv = ast::cast_tree<ast::ConstantLit>(send->recv.get());
    if (recv && recv->symbol == core::Symbols::T_Sig_WithoutRuntime()) {
        return true;
    }

    return false;
}

ParsedSig TypeSyntax::parseSig(core::MutableContext ctx, ast::Send *sigSend, const ParsedSig *parent,
                               bool allowSelfType, core::SymbolRef untypedBlame) {
    ParsedSig sig;

    vector<ast::Send *> sends;

    if (isTProc(ctx, sigSend)) {
        sends.emplace_back(sigSend);
    } else {
        sig.seen.sig = true;
        ENFORCE(sigSend->fun == core::Names::sig());
        auto block = ast::cast_tree<ast::Block>(sigSend->block.get());
        ENFORCE(block);
        auto send = ast::cast_tree<ast::Send>(block->body.get());
        if (send) {
            sends.emplace_back(send);
        } else {
            auto insseq = ast::cast_tree<ast::InsSeq>(block->body.get());
            if (insseq) {
                for (auto &stat : insseq->stats) {
                    send = ast::cast_tree<ast::Send>(stat.get());
                    if (!send) {
                        return sig;
                    }
                    sends.emplace_back(send);
                }
                send = ast::cast_tree<ast::Send>(insseq->expr.get());
                if (!send) {
                    return sig;
                }
                sends.emplace_back(send);
            } else {
                return sig;
            }
        }
    }
    ENFORCE(!sends.empty());

    for (auto &send : sends) {
        ast::Send *tsend = send;
        // extract type parameters early
        while (tsend != nullptr) {
            if (tsend->fun == core::Names::typeParameters()) {
                if (parent != nullptr) {
                    if (auto e = ctx.state.beginError(tsend->loc, core::errors::Resolver::InvalidMethodSignature)) {
                        e.setHeader("Malformed signature; Type parameters can only be specified in outer sig");
                    }
                    break;
                }
                for (auto &arg : tsend->args) {
                    if (auto c = ast::cast_tree<ast::Literal>(arg.get())) {
                        if (c->isSymbol(ctx)) {
                            auto name = c->asSymbol(ctx);
                            auto &typeArgSpec = sig.enterTypeArgByName(name);
                            if (typeArgSpec.type) {
                                if (auto e = ctx.state.beginError(arg->loc,
                                                                  core::errors::Resolver::InvalidMethodSignature)) {
                                    e.setHeader("Malformed signature; Type argument `{}` was specified twice",
                                                name.show(ctx));
                                }
                            }
                            typeArgSpec.type = core::make_type<core::TypeVar>(core::Symbols::todo());
                            typeArgSpec.loc = arg->loc;
                        } else {
                            if (auto e =
                                    ctx.state.beginError(arg->loc, core::errors::Resolver::InvalidMethodSignature)) {
                                e.setHeader("Malformed signature; Type parameters are specified with symbols");
                            }
                        }
                    } else {
                        if (auto e = ctx.state.beginError(arg->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Malformed signature; Type parameters are specified with symbols");
                        }
                    }
                }
            }
            tsend = ast::cast_tree<ast::Send>(tsend->recv.get());
        }
    }
    if (parent == nullptr) {
        parent = &sig;
    }

    for (auto &send : sends) {
        while (send != nullptr) {
            // so we don't report multiple "method does not exist" errors arising from the same expression
            bool reportedInvalidMethod = false;
            switch (send->fun._id) {
                case core::Names::proc()._id:
                    sig.seen.proc = true;
                    break;
                case core::Names::bind()._id: {
                    if (sig.seen.bind) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Malformed `{}`: Multiple calls to `.bind`", send->fun.show(ctx));
                        }
                        sig.bind = core::SymbolRef();
                    }
                    sig.seen.bind = true;

                    if (send->args.size() != 1) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Wrong number of args to `{}`. Expected: `{}`, got: `{}`", "bind", 1,
                                        send->args.size());
                        }
                        break;
                    }

                    auto bind = getResultType(ctx, *(send->args.front()), *parent, allowSelfType, untypedBlame);
                    auto classType = core::cast_type<core::ClassType>(bind.get());
                    if (!classType) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Malformed `{}`: Can only bind to simple class names", send->fun.show(ctx));
                        }
                    } else {
                        sig.bind = classType->symbol;
                    }

                    break;
                }
                case core::Names::params()._id: {
                    if (sig.seen.params) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Malformed `{}`: Multiple calls to `.params`", send->fun.show(ctx));
                        }
                        sig.argTypes.clear();
                    }
                    sig.seen.params = true;

                    if (send->args.empty()) {
                        break;
                    }

                    if (send->args.size() > 1) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Wrong number of args to `{}`. Expected: `{}`, got: `{}`", send->fun.show(ctx),
                                        "0-1", send->args.size());
                        }
                    }

                    auto *hash = ast::cast_tree<ast::Hash>(send->args[0].get());
                    if (hash == nullptr) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            auto paramsStr = send->fun.show(ctx);
                            e.setHeader("`{}` expects keyword arguments", paramsStr);
                            e.addErrorSection(core::ErrorSection(core::ErrorColors::format(
                                "All parameters must be given names in `{}` even if they are positional", paramsStr)));
                        }
                        break;
                    }

                    int i = 0;
                    for (auto &key : hash->keys) {
                        auto &value = hash->values[i++];
                        auto lit = ast::cast_tree<ast::Literal>(key.get());
                        if (lit && lit->isSymbol(ctx)) {
                            core::NameRef name = lit->asSymbol(ctx);
                            auto resultAndBind =
                                getResultTypeAndBind(ctx, *value, *parent, allowSelfType, true, untypedBlame);
                            sig.argTypes.emplace_back(
                                ParsedSig::ArgSpec{key->loc, name, resultAndBind.type, resultAndBind.rebind});
                        }
                    }
                    break;
                }
                case core::Names::typeParameters()._id:
                    // was handled above
                    break;
                case core::Names::abstract()._id:
                    sig.seen.abstract = true;
                    break;
                case core::Names::override_()._id:
                    sig.seen.override_ = true;
                    break;
                case core::Names::implementation()._id:
                    sig.seen.implementation = true;
                    break;
                case core::Names::incompatible_override()._id:
                    sig.seen.incompatibleOverride = true;
                    break;
                case core::Names::overridable()._id:
                    sig.seen.overridable = true;
                    break;
                case core::Names::final()._id:
                    sig.seen.final = true;
                    break;
                case core::Names::returns()._id: {
                    sig.seen.returns = true;
                    if (send->args.size() != 1) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Wrong number of args to `{}`. Expected: `{}`, got: `{}`", "returns", 1,
                                        send->args.size());
                        }
                        break;
                    }

                    auto nil = ast::cast_tree<ast::Literal>(send->args[0].get());
                    if (nil && nil->isNil(ctx)) {
                        if (auto e = ctx.state.beginError(send->args[0]->loc,
                                                          core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("You probably meant .returns(NilClass)");
                        }
                        sig.returns = core::Types::nilClass();
                        break;
                    }

                    sig.returns = getResultType(ctx, *(send->args.front()), *parent, allowSelfType, untypedBlame);

                    break;
                }
                case core::Names::void_()._id:
                    sig.seen.void_ = true;
                    sig.returns = core::Types::void_();
                    break;
                case core::Names::checked()._id:
                    sig.seen.checked = true;
                    break;
                case core::Names::soft()._id:
                    break;
                case core::Names::generated()._id:
                    sig.seen.generated = true;
                    break;
                default:
                    if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                        reportedInvalidMethod = true;
                        e.setHeader("Malformed signature: `{}` is invalid in this context", send->fun.show(ctx));
                        e.addErrorLine(send->loc, "Consult https://sorbet.org/docs/sigs for signature syntax");
                    }
            }
            auto recv = ast::cast_tree<ast::Send>(send->recv.get());

            // we only report this error if we haven't reported another unknown method error
            if (!recv && !reportedInvalidMethod) {
                if (!send->recv->isSelfReference()) {
                    if (!sig.seen.proc) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMethodSignature)) {
                            e.setHeader("Malformed signature: `{}` being invoked on an invalid receiver",
                                        send->fun.show(ctx));
                        }
                    }
                }
                break;
            }

            send = recv;
        }
    }
    ENFORCE(sig.seen.sig || sig.seen.proc);

    return sig;
}

core::TypePtr interpretTCombinator(core::MutableContext ctx, ast::Send *send, const ParsedSig &sig, bool allowSelfType,
                                   core::SymbolRef untypedBlame) {
    switch (send->fun._id) {
        case core::Names::nilable()._id:
            if (send->args.size() != 1) {
                return core::Types::untypedUntracked(); // error will be reported in infer.
            }
            return core::Types::any(ctx,
                                    TypeSyntax::getResultType(ctx, *(send->args[0]), sig, allowSelfType, untypedBlame),
                                    core::Types::nilClass());
        case core::Names::all()._id: {
            if (send->args.empty()) {
                // Error will be reported in infer
                return core::Types::untypedUntracked();
            }
            auto result = TypeSyntax::getResultType(ctx, *(send->args[0]), sig, allowSelfType, untypedBlame);
            int i = 1;
            while (i < send->args.size()) {
                result = core::Types::all(
                    ctx, result, TypeSyntax::getResultType(ctx, *(send->args[i]), sig, allowSelfType, untypedBlame));
                i++;
            }
            return result;
        }
        case core::Names::any()._id: {
            if (send->args.empty()) {
                // Error will be reported in infer
                return core::Types::untypedUntracked();
            }
            auto result = TypeSyntax::getResultType(ctx, *(send->args[0]), sig, allowSelfType, untypedBlame);
            int i = 1;
            while (i < send->args.size()) {
                result = core::Types::any(
                    ctx, result, TypeSyntax::getResultType(ctx, *(send->args[i]), sig, allowSelfType, untypedBlame));
                i++;
            }
            return result;
        }
        case core::Names::typeParameter()._id: {
            if (send->args.size() != 1) {
                // Error will be reported in infer
                return core::Types::untypedUntracked();
            }
            auto arr = ast::cast_tree<ast::Literal>(send->args[0].get());
            if (!arr || !arr->isSymbol(ctx)) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("type_parameter requires a symbol");
                }
                return core::Types::untypedUntracked();
            }
            auto fnd = sig.findTypeArgByName(arr->asSymbol(ctx));
            if (!fnd.type) {
                if (auto e = ctx.state.beginError(arr->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Unspecified type parameter");
                }
                return core::Types::untypedUntracked();
            }
            return fnd.type;
        }
        case core::Names::enum_()._id: {
            if (send->args.size() != 1) {
                // Error will be reported in infer
                return core::Types::untypedUntracked();
            }
            auto arr = ast::cast_tree<ast::Array>(send->args[0].get());
            if (arr == nullptr) {
                // TODO(pay-server) unsilence this error and support enums from pay-server
                { return core::Types::Object(); }
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("enum must be passed a literal array. e.g. enum([1,\"foo\",MyClass])");
                }
                return core::Types::untypedUntracked();
            }
            if (arr->elems.empty()) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("enum([]) is invalid");
                }
                return core::Types::untypedUntracked();
            }
            auto result = getResultLiteral(ctx, arr->elems[0]);
            int i = 1;
            while (i < arr->elems.size()) {
                result = core::Types::any(ctx, result, getResultLiteral(ctx, arr->elems[i]));
                i++;
            }
            return result;
        }
        case core::Names::classOf()._id: {
            if (send->args.size() != 1) {
                // Error will be reported in infer
                return core::Types::untypedUntracked();
            }

            auto arg = send->args[0].get();

            auto *obj = ast::cast_tree<ast::ConstantLit>(arg);
            if (!obj) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("T.class_of needs a Class as its argument");
                }
                return core::Types::untypedUntracked();
            }
            auto maybeAliased = obj->symbol;
            if (maybeAliased.data(ctx)->isTypeAlias()) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("T.class_of can't be used with a T.type_alias");
                }
                return core::Types::untypedUntracked();
            }
            if (maybeAliased.data(ctx)->isTypeMember()) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("T.class_of can't be used with a T.type_member");
                }
                return core::Types::untypedUntracked();
            }
            auto sym = maybeAliased.data(ctx)->dealias(ctx);
            if (sym.data(ctx)->isStaticField()) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("T.class_of can't be used with a constant field");
                }
                return core::Types::untypedUntracked();
            }

            auto singleton = sym.data(ctx)->singletonClass(ctx);
            if (!singleton.exists()) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Unknown class");
                }
                return core::Types::untypedUntracked();
            }
            return core::make_type<core::ClassType>(singleton);
        }
        case core::Names::untyped()._id:
            return core::Types::untyped(ctx, untypedBlame);
        case core::Names::selfType()._id:
            if (allowSelfType) {
                return core::make_type<core::SelfType>();
            }
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                e.setHeader("Only top-level T.self_type is supported");
            }
            return core::Types::untypedUntracked();
        case core::Names::noreturn()._id:
            return core::Types::bottom();

        default:
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                e.setHeader("Unsupported method `{}`", "T." + send->fun.show(ctx));
            }
            return core::Types::untypedUntracked();
    }
}

core::TypePtr TypeSyntax::getResultType(core::MutableContext ctx, ast::Expression &expr,
                                        const ParsedSig &sigBeingParsed, bool allowSelfType,
                                        core::SymbolRef untypedBlame) {
    return getResultTypeAndBind(ctx, expr, sigBeingParsed, allowSelfType, false, untypedBlame).type;
}

TypeSyntax::ResultType TypeSyntax::getResultTypeAndBind(core::MutableContext ctx, ast::Expression &expr,
                                                        const ParsedSig &sigBeingParsed, bool allowSelfType,
                                                        bool allowRebind, core::SymbolRef untypedBlame) {
    // Ensure that we only check types from a class context
    auto ctxOwnerData = ctx.owner.data(ctx);
    ENFORCE(ctxOwnerData->isClass(), "getResultTypeAndBind wasn't called with a class owner");

    ResultType result;
    typecase(
        &expr,
        [&](ast::Array *arr) {
            vector<core::TypePtr> elems;
            for (auto &el : arr->elems) {
                elems.emplace_back(getResultType(ctx, *el, sigBeingParsed, false, untypedBlame));
            }
            result.type = core::TupleType::build(ctx, elems);
        },
        [&](ast::Hash *hash) {
            vector<core::TypePtr> keys;
            vector<core::TypePtr> values;

            for (auto &ktree : hash->keys) {
                auto &vtree = hash->values[&ktree - &hash->keys.front()];
                auto val = getResultType(ctx, *vtree, sigBeingParsed, false, untypedBlame);
                auto lit = ast::cast_tree<ast::Literal>(ktree.get());
                if (lit && (lit->isSymbol(ctx) || lit->isString(ctx))) {
                    ENFORCE(core::cast_type<core::LiteralType>(lit->value.get()));
                    keys.emplace_back(lit->value);
                    values.emplace_back(val);
                } else {
                    if (auto e = ctx.state.beginError(ktree->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                        e.setHeader("Malformed type declaration. Shape keys must be literals");
                    }
                }
            }
            result.type = core::make_type<core::ShapeType>(core::Types::hashOfUntyped(), keys, values);
        },
        [&](ast::ConstantLit *i) {
            auto maybeAliased = i->symbol;
            ENFORCE(maybeAliased.exists());

            if (maybeAliased.data(ctx)->isTypeAlias()) {
                result.type = maybeAliased.data(ctx)->resultType;
                return;
            }
            bool silenceGenericError = maybeAliased == core::Symbols::Hash() ||
                                       maybeAliased == core::Symbols::Array() || maybeAliased == core::Symbols::Set() ||
                                       maybeAliased == core::Symbols::Struct() || maybeAliased == core::Symbols::File();
            // TODO: reduce this^^^ set.
            auto sym = maybeAliased.data(ctx)->dealias(ctx);
            if (sym.data(ctx)->isClass()) {
                if (sym.data(ctx)->typeArity(ctx) > 0 && !silenceGenericError) {
                    if (auto e = ctx.state.beginError(i->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                        e.setHeader("Malformed type declaration. Generic class without type arguments `{}`",
                                    maybeAliased.show(ctx));
                    }
                }
                if (sym == core::Symbols::StubModule()) {
                    // Though for normal types _and_ stub types `infer` should use `externalType`,
                    // using `externalType` for stub types here will lead to incorrect handling of global state hashing,
                    // where we won't see difference between two different unresolved stubs(or a mistyped stub). thus,
                    // while normally we would treat stubs as untyped, in `sig`s we treat them as proper types, so that
                    // we can correctly hash them.
                    auto unresolvedPath = i->fullUnresolvedPath(ctx);
                    ENFORCE(unresolvedPath.has_value());
                    result.type =
                        core::make_type<core::UnresolvedClassType>(unresolvedPath->first, move(unresolvedPath->second));
                } else {
                    result.type = sym.data(ctx)->externalType(ctx);
                }
            } else if (sym.data(ctx)->isTypeMember()) {
                auto symData = sym.data(ctx);
                auto symOwner = symData->owner.data(ctx);

                bool isTypeTemplate = symOwner->isSingletonClass(ctx);
                bool ctxIsSingleton = ctxOwnerData->isSingletonClass(ctx);

                // Check if we're processing a type within the class that
                // defines this type member by comparing the singleton class of
                // the context, and the singleton class of the type member's
                // owner.
                core::SymbolRef symOwnerSingleton =
                    isTypeTemplate ? symData->owner : symOwner->lookupSingletonClass(ctx);
                core::SymbolRef ctxSingleton = ctxIsSingleton ? ctx.owner : ctxOwnerData->lookupSingletonClass(ctx);
                bool usedOnSourceClass = symOwnerSingleton == ctxSingleton;

                // For this to be a valid use of a member or template type, this
                // must:
                //
                // 1. be used in the context of the class that defines it
                // 2. if it's a type_template type, be used in a singleton
                //    method
                // 3. if it's a type_member type, be used in an instance method
                if (usedOnSourceClass && ((isTypeTemplate && ctxIsSingleton) || !(isTypeTemplate || ctxIsSingleton))) {
                    result.type = core::make_type<core::LambdaParam>(sym);
                } else {
                    if (auto e = ctx.state.beginError(i->loc, core::errors::Resolver::InvalidTypeDeclarationTyped)) {
                        string typeSource = isTypeTemplate ? "type_template" : "type_member";
                        string typeStr = sym.show(ctx);

                        if (usedOnSourceClass) {
                            if (ctxIsSingleton) {
                                e.setHeader("`{}` type `{}` used in a singleton method definition", typeSource,
                                            typeStr);
                            } else {
                                e.setHeader("`{}` type `{}` used in an instance method definition", typeSource,
                                            typeStr);
                            }
                        } else {
                            e.setHeader("`{}` type `{}` used outside of the class definition", typeSource, typeStr);
                        }
                    }
                    result.type = core::Types::untypedUntracked();
                }
            } else if (sym.data(ctx)->isStaticField()) {
                if (auto e = ctx.state.beginError(i->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Constant `{}` is not a class or type alias", maybeAliased.show(ctx));
                    e.addErrorLine(sym.data(ctx)->loc(),
                                   "If you are trying to define a type alias, you should use `{}` here",
                                   "T.type_alias");
                }
                result.type = core::Types::untypedUntracked();
            } else {
                if (auto e = ctx.state.beginError(i->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Malformed type declaration. Not a class type `{}`", maybeAliased.show(ctx));
                }
                result.type = core::Types::untypedUntracked();
            }
        },
        [&](ast::Send *s) {
            if (isTProc(ctx, s)) {
                auto sig = parseSig(ctx, s, &sigBeingParsed, false, untypedBlame);
                if (sig.bind.exists()) {
                    if (!allowRebind) {
                        if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                            e.setHeader("Using `bind` is not permitted here");
                        }
                    } else {
                        result.rebind = sig.bind;
                    }
                }

                vector<core::TypePtr> targs;

                if (sig.returns == nullptr) {
                    if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                        e.setHeader("Malformed T.proc: You must specify a return type");
                    }
                    targs.emplace_back(core::Types::untypedUntracked());
                } else {
                    targs.emplace_back(sig.returns);
                }

                for (auto &arg : sig.argTypes) {
                    targs.emplace_back(arg.type);
                }

                auto arity = targs.size() - 1;
                if (arity > core::Symbols::MAX_PROC_ARITY) {
                    if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                        e.setHeader("Malformed T.proc: Too many arguments (max `{}`)", core::Symbols::MAX_PROC_ARITY);
                    }
                    result.type = core::Types::untypedUntracked();
                    return;
                }
                auto sym = core::Symbols::Proc(arity);

                result.type = core::make_type<core::AppliedType>(sym, targs);
                return;
            }
            auto recv = s->recv.get();

            auto *recvi = ast::cast_tree<ast::ConstantLit>(recv);
            if (recvi == nullptr) {
                if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Malformed type declaration. Unknown type syntax. Expected a ClassName or T.<func>");
                }
                result.type = core::Types::untypedUntracked();
                return;
            }
            if (recvi->symbol == core::Symbols::T()) {
                result.type = interpretTCombinator(ctx, s, sigBeingParsed, allowSelfType, untypedBlame);
                return;
            }

            if (recvi->symbol == core::Symbols::Magic() && s->fun == core::Names::callWithSplat()) {
                // TODO(pay-server) remove this block
                if (auto e = ctx.state.beginError(recvi->loc, core::errors::Resolver::InvalidTypeDeclarationTyped)) {
                    e.setHeader("Splats are unsupported by the static checker and banned in typed code");
                }
                result.type = core::Types::untypedUntracked();
                return;
            }

            if (s->fun != core::Names::squareBrackets()) {
                if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Malformed type declaration. Unknown type syntax. Expected a ClassName or T.<func>");
                }
                result.type = core::Types::untypedUntracked();
                return;
            }

            InlinedVector<unique_ptr<core::TypeAndOrigins>, 2> holders;
            InlinedVector<const core::TypeAndOrigins *, 2> targs;
            InlinedVector<core::Loc, 2> argLocs;
            targs.reserve(s->args.size());
            argLocs.reserve(s->args.size());
            holders.reserve(s->args.size());
            for (auto &arg : s->args) {
                core::TypeAndOrigins ty;
                ty.origins.emplace_back(arg->loc);
                ty.type = core::make_type<core::MetaType>(
                    TypeSyntax::getResultType(ctx, *arg, sigBeingParsed, false, untypedBlame));
                holders.emplace_back(make_unique<core::TypeAndOrigins>(move(ty)));
                targs.emplace_back(holders.back().get());
                argLocs.emplace_back(arg->loc);
            }

            core::SymbolRef corrected;
            if (recvi->symbol == core::Symbols::Array()) {
                corrected = core::Symbols::T_Array();
            } else if (recvi->symbol == core::Symbols::Hash()) {
                corrected = core::Symbols::T_Hash();
            } else if (recvi->symbol == core::Symbols::Enumerable()) {
                corrected = core::Symbols::T_Enumerable();
            } else if (recvi->symbol == core::Symbols::Enumerator()) {
                corrected = core::Symbols::T_Enumerator();
            } else if (recvi->symbol == core::Symbols::Range()) {
                corrected = core::Symbols::T_Range();
            } else if (recvi->symbol == core::Symbols::Set()) {
                corrected = core::Symbols::T_Set();
            }
            if (corrected.exists()) {
                if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::BadStdlibGeneric)) {
                    e.setHeader("Use `{}`, not `{}` to declare a typed `{}`", corrected.data(ctx)->show(ctx) + "[...]",
                                recvi->symbol.data(ctx)->show(ctx) + "[...]", recvi->symbol.data(ctx)->show(ctx));
                    e.addErrorSection(
                        core::ErrorSection(core::ErrorColors::format("`{}` will not work in the runtime type system.",
                                                                     recvi->symbol.data(ctx)->show(ctx) + "[...]")));
                }
                result.type = core::Types::untypedUntracked();
                return;
            } else {
                corrected = recvi->symbol;
            }
            corrected = corrected.data(ctx)->dealias(ctx);

            if (!corrected.data(ctx)->isClass()) {
                if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Expected a class or module");
                }
                result.type = core::Types::untypedUntracked();
                return;
            }

            auto ctype = core::make_type<core::ClassType>(corrected.data(ctx)->singletonClass(ctx));
            core::CallLocs locs{
                s->loc,
                recvi->loc,
                argLocs,
            };
            core::DispatchArgs dispatchArgs{core::Names::squareBrackets(), locs, targs, ctype, ctype, nullptr};
            auto dispatched = ctype->dispatchCall(ctx, dispatchArgs);
            for (auto &comp : dispatched.components) {
                for (auto &err : comp.errors) {
                    ctx.state._error(move(err));
                }
            }
            auto out = dispatched.returnType;

            if (out->isUntyped()) {
                result.type = out;
                return;
            }
            if (auto *mt = core::cast_type<core::MetaType>(out.get())) {
                result.type = mt->wrapped;
                return;
            }

            if (auto e = ctx.state.beginError(s->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                e.setHeader("Malformed type declaration. Unknown type syntax. Expected a ClassName or T.<func>");
            }
            result.type = core::Types::untypedUntracked();
        },
        [&](ast::Local *slf) {
            if (slf->isSelfReference()) {
                result.type = ctxOwnerData->selfType(ctx);
            } else {
                if (auto e = ctx.state.beginError(slf->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                    e.setHeader("Unsupported type syntax");
                }
                result.type = core::Types::untypedUntracked();
            }
        },
        [&](ast::Expression *expr) {
            if (auto e = ctx.state.beginError(expr->loc, core::errors::Resolver::InvalidTypeDeclaration)) {
                e.setHeader("Unsupported type syntax");
            }
            result.type = core::Types::untypedUntracked();
        });
    ENFORCE(result.type.get() != nullptr);
    result.type->sanityCheck(ctx);
    return result;
}

ParsedSig::TypeArgSpec &ParsedSig::enterTypeArgByName(core::NameRef name) {
    for (auto &current : typeArgs) {
        if (current.name == name) {
            return current;
        }
    }
    auto &inserted = typeArgs.emplace_back();
    inserted.name = name;
    return inserted;
}

const ParsedSig::TypeArgSpec emptyTypeArgSpec;

const ParsedSig::TypeArgSpec &ParsedSig::findTypeArgByName(core::NameRef name) const {
    for (auto &current : typeArgs) {
        if (current.name == name) {
            return current;
        }
    }
    return emptyTypeArgSpec;
}
} // namespace sorbet::resolver
