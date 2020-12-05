/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ClauseTranslator.h
 *
 * Translator for clauses from AST to RAM
 *
 ***********************************************************************/

#include "ast2ram/ClauseTranslator.h"
#include "Global.h"
#include "LogStatement.h"
#include "ast/Aggregator.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/IntrinsicFunctor.h"
#include "ast/NilConstant.h"
#include "ast/NumericConstant.h"
#include "ast/RecordInit.h"
#include "ast/Relation.h"
#include "ast/StringConstant.h"
#include "ast/UnnamedVariable.h"
#include "ast/analysis/Functor.h"
#include "ast/analysis/PolymorphicObjects.h"
#include "ast/transform/ReorderLiterals.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "ast2ram/AstToRamTranslator.h"
#include "ast2ram/ConstraintTranslator.h"
#include "ast2ram/ValueTranslator.h"
#include "ast2ram/utility/Location.h"
#include "ast2ram/utility/TranslatorContext.h"
#include "ast2ram/utility/Utils.h"
#include "ast2ram/utility/ValueIndex.h"
#include "ram/Aggregate.h"
#include "ram/Break.h"
#include "ram/Conjunction.h"
#include "ram/Constraint.h"
#include "ram/DebugInfo.h"
#include "ram/EmptinessCheck.h"
#include "ram/ExistenceCheck.h"
#include "ram/Filter.h"
#include "ram/FloatConstant.h"
#include "ram/LogRelationTimer.h"
#include "ram/LogSize.h"
#include "ram/LogTimer.h"
#include "ram/Negation.h"
#include "ram/NestedIntrinsicOperator.h"
#include "ram/Project.h"
#include "ram/Query.h"
#include "ram/Relation.h"
#include "ram/Scan.h"
#include "ram/Sequence.h"
#include "ram/SignedConstant.h"
#include "ram/TupleElement.h"
#include "ram/UnpackRecord.h"
#include "ram/UnsignedConstant.h"
#include "ram/utility/Utils.h"
#include "souffle/SymbolTable.h"
#include "souffle/utility/StringUtil.h"
#include <map>
#include <vector>

namespace souffle::ast2ram {

Own<ram::Statement> ClauseTranslator::generateClause(const TranslatorContext& context,
        SymbolTable& symbolTable, const ast::Clause& clause, const ast::Clause& originalClause, int version) {
    return ClauseTranslator(context, symbolTable).translateClause(clause, originalClause, version);
}

VecOwn<ram::Statement> ClauseTranslator::generateClauseVersions(const TranslatorContext& context,
        SymbolTable& symbolTable, const std::set<const ast::Relation*>& scc, const ast::Clause* clause) {
    VecOwn<ram::Statement> clauseVersions;

    // Create each version
    int version = 0;
    const auto& atoms = ast::getBodyLiterals<ast::Atom>(*clause);
    for (size_t i = 0; i < atoms.size(); i++) {
        const auto* atom = atoms[i];

        // Only interested in atoms within the same SCC
        if (!contains(scc, context.getAtomRelation(atom))) {
            continue;
        }

        // Replace wildcards with variables to reduce indices
        // nameUnnamedVariables(clause);

        auto translatedClause =
                ClauseTranslator(context, symbolTable).generateClauseVersion(scc, clause, i, version);
        appendStmt(clauseVersions, std::move(translatedClause));

        // increment version counter
        version++;
    }

    // Check that the correct number of versions have been created
    if (clause->getExecutionPlan() != nullptr) {
        int maxVersion = -1;
        for (auto const& cur : clause->getExecutionPlan()->getOrders()) {
            maxVersion = std::max(cur.first, maxVersion);
        }
        assert(version > maxVersion && "missing clause versions");
    }

    return clauseVersions;
}

Own<ram::Statement> ClauseTranslator::generateClauseVersion(const std::set<const ast::Relation*>& scc,
        const ast::Clause* clause, size_t deltaAtomIdx, size_t version) {
    const auto& atoms = ast::getBodyLiterals<ast::Atom>(*clause);

    // Update delta atom
    deltaAtom = atoms.at(deltaAtomIdx);

    // Update prevs list
    prevs.clear();
    for (size_t j = deltaAtomIdx + 1; j < atoms.size(); j++) {
        const auto* atomRelation = context.getAtomRelation(atoms[j]);
        if (contains(scc, atomRelation)) {
            prevs.push_back(atoms[j]);
        }
    }

    // Translate the resultant clause as would be done normally
    Own<ram::Statement> rule = translateClause(*clause, *clause, version);

    // Add loging
    if (Global::config().has("profile")) {
        const std::string& relationName = toString(clause->getHead()->getQualifiedName());
        const auto& srcLocation = clause->getSrcLoc();
        const std::string clauseText = stringify(toString(*clause));
        const std::string logTimerStatement =
                LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
        const std::string logSizeStatement =
                LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
        rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement,
                getNewRelationName(clause->getHead()->getQualifiedName()));
    }

    // Add debug info
    std::ostringstream ds;
    ds << toString(*clause) << "\nin file ";
    ds << clause->getSrcLoc();
    rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

    // Add to loop body
    return mk<ram::Sequence>(std::move(rule));
}

Own<ram::Statement> ClauseTranslator::translateClause(
        const ast::Clause& clause, const ast::Clause& originalClause, int version) {
    // Create the appropriate query
    if (isFact(clause)) {
        return createRamFactQuery(clause);
    }
    return createRamRuleQuery(clause, originalClause, version);
}

std::string ClauseTranslator::getClauseAtomName(const ast::Clause& clause, const ast::Atom* atom) const {
    if (!isRecursive()) {
        return getConcreteRelationName(atom->getQualifiedName());
    }
    if (clause.getHead() == atom) {
        return getNewRelationName(atom->getQualifiedName());
    }
    if (deltaAtom == atom) {
        return getDeltaRelationName(atom->getQualifiedName());
    }
    return getConcreteRelationName(atom->getQualifiedName());
}

Own<ram::Statement> ClauseTranslator::createRamFactQuery(const ast::Clause& clause) const {
    assert(isFact(clause) && "clause should be fact");
    assert(!isRecursive() && "recursive clauses cannot have facts");
    const auto* head = clause.getHead();

    // Translate arguments
    VecOwn<ram::Expression> values;
    for (auto& arg : head->getArguments()) {
        values.push_back(ValueTranslator::translate(context, symbolTable, ValueIndex(), arg));
    }

    // Create a fact statement
    return mk<ram::Query>(mk<ram::Project>(getClauseAtomName(clause, head), std::move(values)));
}

Own<ram::Statement> ClauseTranslator::createRamRuleQuery(
        const ast::Clause& clause, const ast::Clause& originalClause, int version) {
    assert(isRule(clause) && "clause should be rule");

    // Set up atom ordering
    atomOrder = getAtomOrdering(clause, version);

    // Index all variables and generators in the clause
    indexClause(clause);

    // Set up the RAM statement bottom-up
    auto op = createProjection(clause);
    op = addVariableBindingConstraints(std::move(op));
    op = addBodyLiteralConstraints(clause, std::move(op));
    op = addGeneratorLevels(std::move(op), clause);
    op = addVariableIntroductions(clause, originalClause, version, std::move(op));
    op = addEntryPoint(originalClause, std::move(op));
    return mk<ram::Query>(std::move(op));
}

Own<ram::Operation> ClauseTranslator::addEntryPoint(
        const ast::Clause& originalClause, Own<ram::Operation> op) const {
    auto cond = createCondition(originalClause);
    return cond != nullptr ? mk<ram::Filter>(std::move(cond), std::move(op)) : std::move(op);
}

Own<ram::Operation> ClauseTranslator::addVariableBindingConstraints(Own<ram::Operation> op) const {
    for (const auto& [_, references] : valueIndex->getVariableReferences()) {
        // Equate the first appearance to all other appearances
        assert(!references.empty() && "variable should appear at least once");
        const auto& first = *references.begin();
        for (const auto& reference : references) {
            if (first != reference && !valueIndex->isGenerator(reference.identifier)) {
                // TODO: float type equivalence check
                op = addEqualityCheck(
                        std::move(op), makeRamTupleElement(first), makeRamTupleElement(reference), false);
            }
        }
    }
    return op;
}

Own<ram::Operation> ClauseTranslator::createProjection(const ast::Clause& clause) const {
    const auto head = clause.getHead();
    auto headRelationName = getClauseAtomName(clause, head);

    VecOwn<ram::Expression> values;
    for (const auto* arg : head->getArguments()) {
        values.push_back(ValueTranslator::translate(context, symbolTable, *valueIndex, arg));
    }

    Own<ram::Operation> project = mk<ram::Project>(headRelationName, std::move(values));

    if (head->getArity() == 0) {
        project = mk<ram::Filter>(mk<ram::EmptinessCheck>(headRelationName), std::move(project));
    }

    // build up insertion call
    return project;  // start with innermost
}

Own<ram::Operation> ClauseTranslator::addAtomScan(Own<ram::Operation> op, const ast::Atom* atom,
        const ast::Clause& clause, const ast::Clause& originalClause, int curLevel, int version) const {
    const ast::Atom* head = clause.getHead();

    // add constraints
    op = addConstantConstraints(curLevel, atom->getArguments(), std::move(op));

    // add check for emptiness for an atom
    op = mk<ram::Filter>(
            mk<ram::Negation>(mk<ram::EmptinessCheck>(getClauseAtomName(clause, atom))), std::move(op));

    // check whether all arguments are unnamed variables
    bool isAllArgsUnnamed = all_of(
            atom->getArguments(), [&](const ast::Argument* arg) { return isA<ast::UnnamedVariable>(arg); });

    // add a scan level
    if (atom->getArity() != 0 && !isAllArgsUnnamed) {
        if (head->getArity() == 0) {
            op = mk<ram::Break>(mk<ram::Negation>(mk<ram::EmptinessCheck>(getClauseAtomName(clause, head))),
                    std::move(op));
        }

        std::stringstream ss;
        if (Global::config().has("profile")) {
            ss << "@frequency-atom" << ';';
            ss << originalClause.getHead()->getQualifiedName() << ';';
            ss << version << ';';
            ss << stringify(toString(clause)) << ';';
            ss << stringify(toString(*atom)) << ';';
            ss << stringify(toString(originalClause)) << ';';
            ss << curLevel << ';';
        }
        op = mk<ram::Scan>(getClauseAtomName(clause, atom), curLevel, std::move(op), ss.str());
    }

    return op;
}

Own<ram::Operation> ClauseTranslator::addRecordUnpack(
        Own<ram::Operation> op, const ast::RecordInit* rec, int curLevel) const {
    // add constant constraints
    op = addConstantConstraints(curLevel, rec->getArguments(), std::move(op));

    // add an unpack level
    const Location& loc = valueIndex->getDefinitionPoint(*rec);
    op = mk<ram::UnpackRecord>(std::move(op), curLevel, makeRamTupleElement(loc), rec->getArguments().size());
    return op;
}

Own<ram::Operation> ClauseTranslator::addVariableIntroductions(
        const ast::Clause& clause, const ast::Clause& originalClause, int version, Own<ram::Operation> op) {
    for (int i = operators.size() - 1; i >= 0; i--) {
        const auto* curOp = operators.at(i);
        if (const auto* atom = dynamic_cast<const ast::Atom*>(curOp)) {
            // add atom arguments through a scan
            op = addAtomScan(std::move(op), atom, clause, originalClause, i, version);
        } else if (const auto* rec = dynamic_cast<const ast::RecordInit*>(curOp)) {
            // add record arguments through an unpack
            op = addRecordUnpack(std::move(op), rec, i);
        } else {
            fatal("Unsupported AST node for creation of scan-level!");
        }
    }
    return op;
}

Own<ram::Operation> ClauseTranslator::instantiateAggregator(
        Own<ram::Operation> op, const ast::Clause& clause, const ast::Aggregator* agg, int curLevel) const {
    auto addAggEqCondition = [&](Own<ram::Condition> aggr, Own<ram::Expression> value, size_t pos) {
        if (isUndefValue(value.get())) return aggr;

        // TODO: float type equivalence check
        return addConjunctiveTerm(
                std::move(aggr), mk<ram::Constraint>(BinaryConstraintOp::EQ,
                                         mk<ram::TupleElement>(curLevel, pos), std::move(value)));
    };

    Own<ram::Condition> aggCond;

    // translate constraints of sub-clause
    for (const auto* lit : agg->getBodyLiterals()) {
        // literal becomes a constraint
        if (auto condition = ConstraintTranslator::translate(context, symbolTable, *valueIndex, lit)) {
            aggCond = addConjunctiveTerm(std::move(aggCond), std::move(condition));
        }
    }

    // translate arguments of atom to conditions
    const auto& aggBodyAtoms =
            filter(agg->getBodyLiterals(), [&](const ast::Literal* lit) { return isA<ast::Atom>(lit); });
    assert(aggBodyAtoms.size() == 1 && "exactly one atom should exist per aggregator body");
    const auto* aggAtom = static_cast<const ast::Atom*>(aggBodyAtoms.at(0));

    const auto& aggAtomArgs = aggAtom->getArguments();
    for (size_t i = 0; i < aggAtomArgs.size(); i++) {
        const auto* arg = aggAtomArgs.at(i);

        // variable bindings are issued differently since we don't want self
        // referential variable bindings
        if (auto* var = dynamic_cast<const ast::Variable*>(arg)) {
            for (auto&& loc : valueIndex->getVariableReferences(var->getName())) {
                if (curLevel != loc.identifier || (int)i != loc.element) {
                    aggCond = addAggEqCondition(std::move(aggCond), makeRamTupleElement(loc), i);
                    break;
                }
            }
        } else {
            assert(arg != nullptr && "aggregator argument cannot be nullptr");
            auto value = ValueTranslator::translate(context, symbolTable, *valueIndex, arg);
            aggCond = addAggEqCondition(std::move(aggCond), std::move(value), i);
        }
    }

    // translate aggregate expression
    const auto* aggExpr = agg->getTargetExpression();
    auto expr = aggExpr ? ValueTranslator::translate(context, symbolTable, *valueIndex, aggExpr) : nullptr;

    // add Ram-Aggregation layer
    return mk<ram::Aggregate>(std::move(op), agg->getFinalType().value(), getClauseAtomName(clause, aggAtom),
            expr ? std::move(expr) : mk<ram::UndefValue>(), aggCond ? std::move(aggCond) : mk<ram::True>(),
            curLevel);
}

Own<ram::Operation> ClauseTranslator::instantiateMultiResultFunctor(
        Own<ram::Operation> op, const ast::IntrinsicFunctor* inf, int curLevel) const {
    VecOwn<ram::Expression> args;
    for (auto&& x : inf->getArguments()) {
        args.push_back(ValueTranslator::translate(context, symbolTable, *valueIndex, x));
    }

    auto func_op = [&]() -> ram::NestedIntrinsicOp {
        switch (inf->getFinalOpType().value()) {
            case FunctorOp::RANGE: return ram::NestedIntrinsicOp::RANGE;
            case FunctorOp::URANGE: return ram::NestedIntrinsicOp::URANGE;
            case FunctorOp::FRANGE: return ram::NestedIntrinsicOp::FRANGE;

            default: fatal("missing case handler or bad code-gen");
        }
    };

    return mk<ram::NestedIntrinsicOperator>(func_op(), std::move(args), std::move(op), curLevel);
}

Own<ram::Operation> ClauseTranslator::addGeneratorLevels(
        Own<ram::Operation> op, const ast::Clause& clause) const {
    size_t curLevel = operators.size() + generators.size() - 1;
    for (const auto* generator : reverse(generators)) {
        if (auto agg = dynamic_cast<const ast::Aggregator*>(generator)) {
            op = instantiateAggregator(std::move(op), clause, agg, curLevel);
        } else if (const auto* inf = dynamic_cast<const ast::IntrinsicFunctor*>(generator)) {
            op = instantiateMultiResultFunctor(std::move(op), inf, curLevel);
        } else {
            assert(false && "unhandled generator");
        }
        curLevel--;
    }
    return op;
}

Own<ram::Operation> ClauseTranslator::addNegate(
        const ast::Clause& clause, const ast::Atom* atom, Own<ram::Operation> op, bool isDelta) const {
    size_t auxiliaryArity = context.getEvaluationArity(atom);
    assert(auxiliaryArity <= atom->getArity() && "auxiliary arity out of bounds");
    size_t arity = atom->getArity() - auxiliaryArity;
    std::string name = isDelta ? getDeltaRelationName(atom->getQualifiedName())
                               : getConcreteRelationName(atom->getQualifiedName());

    if (arity == 0) {
        // for a nullary, negation is a simple emptiness check
        return mk<ram::Filter>(mk<ram::EmptinessCheck>(name), std::move(op));
    }

    // else, we construct the atom and create a negation
    VecOwn<ram::Expression> values;
    auto args = atom->getArguments();
    for (size_t i = 0; i < arity; i++) {
        values.push_back(ValueTranslator::translate(context, symbolTable, *valueIndex, args[i]));
    }
    for (size_t i = 0; i < auxiliaryArity; i++) {
        values.push_back(mk<ram::UndefValue>());
    }
    return mk<ram::Filter>(
            mk<ram::Negation>(mk<ram::ExistenceCheck>(name, std::move(values))), std::move(op));
}

Own<ram::Operation> ClauseTranslator::addBodyLiteralConstraints(
        const ast::Clause& clause, Own<ram::Operation> op) const {
    for (const auto* lit : clause.getBodyLiterals()) {
        // constraints become literals
        if (auto condition = ConstraintTranslator::translate(context, symbolTable, *valueIndex, lit)) {
            op = mk<ram::Filter>(std::move(condition), std::move(op));
        }
    }

    if (isRecursive()) {
        if (clause.getHead()->getArity() > 0) {
            // also negate the head
            op = addNegate(clause, clause.getHead(), std::move(op), false);
        }

        // also add in prev stuff
        for (const auto* prev : prevs) {
            op = addNegate(clause, prev, std::move(op), true);
        }
    }

    return op;
}

Own<ram::Condition> ClauseTranslator::createCondition(const ast::Clause& originalClause) const {
    const auto head = originalClause.getHead();

    // add stopping criteria for nullary relations
    // (if it contains already the null tuple, don't re-compute)
    if (head->getArity() == 0) {
        return mk<ram::EmptinessCheck>(getClauseAtomName(originalClause, head));
    }
    return nullptr;
}

RamDomain ClauseTranslator::getConstantRamRepresentation(
        SymbolTable& symbolTable, const ast::Constant& constant) {
    if (auto strConstant = dynamic_cast<const ast::StringConstant*>(&constant)) {
        return symbolTable.lookup(strConstant->getConstant());
    } else if (isA<ast::NilConstant>(&constant)) {
        return 0;
    } else if (auto* numConstant = dynamic_cast<const ast::NumericConstant*>(&constant)) {
        assert(numConstant->getFinalType().has_value() && "constant should have valid type");
        switch (numConstant->getFinalType().value()) {
            case ast::NumericConstant::Type::Int:
                return RamSignedFromString(numConstant->getConstant(), nullptr, 0);
            case ast::NumericConstant::Type::Uint:
                return RamUnsignedFromString(numConstant->getConstant(), nullptr, 0);
            case ast::NumericConstant::Type::Float: return RamFloatFromString(numConstant->getConstant());
        }
    }

    fatal("unaccounted-for constant");
}

Own<ram::Expression> ClauseTranslator::translateConstant(
        SymbolTable& symbolTable, const ast::Constant& constant) {
    auto rawConstant = getConstantRamRepresentation(symbolTable, constant);
    if (const auto* numericConstant = dynamic_cast<const ast::NumericConstant*>(&constant)) {
        switch (numericConstant->getFinalType().value()) {
            case ast::NumericConstant::Type::Int: return mk<ram::SignedConstant>(rawConstant);
            case ast::NumericConstant::Type::Uint: return mk<ram::UnsignedConstant>(rawConstant);
            case ast::NumericConstant::Type::Float: return mk<ram::FloatConstant>(rawConstant);
        }
        fatal("unaccounted-for constant");
    }
    return mk<ram::SignedConstant>(rawConstant);
}

Own<ram::Operation> ClauseTranslator::addEqualityCheck(
        Own<ram::Operation> op, Own<ram::Expression> lhs, Own<ram::Expression> rhs, bool isFloat) const {
    auto eqOp = isFloat ? BinaryConstraintOp::FEQ : BinaryConstraintOp::EQ;
    auto eqConstraint = mk<ram::Constraint>(eqOp, std::move(lhs), std::move(rhs));
    return mk<ram::Filter>(std::move(eqConstraint), std::move(op));
}

Own<ram::Operation> ClauseTranslator::addConstantConstraints(
        size_t curLevel, const std::vector<ast::Argument*>& arguments, Own<ram::Operation> op) const {
    for (size_t i = 0; i < arguments.size(); i++) {
        const auto* argument = arguments.at(i);
        if (const auto* numericConstant = dynamic_cast<const ast::NumericConstant*>(argument)) {
            const auto& finalType = numericConstant->getFinalType();
            assert(finalType.has_value() && "numeric constant not bound to a type");

            bool isFloat = finalType.value() == ast::NumericConstant::Type::Float;
            auto lhs = mk<ram::TupleElement>(curLevel, i);
            auto rhs = translateConstant(symbolTable, *numericConstant);
            op = addEqualityCheck(std::move(op), std::move(lhs), std::move(rhs), isFloat);
        } else if (const auto* constant = dynamic_cast<const ast::Constant*>(argument)) {
            auto lhs = mk<ram::TupleElement>(curLevel, i);
            auto rhs = translateConstant(symbolTable, *constant);
            op = addEqualityCheck(std::move(op), std::move(lhs), std::move(rhs), false);
        }
    }

    return op;
}

std::vector<ast::Atom*> ClauseTranslator::getAtomOrdering(
        const ast::Clause& clause, const int version) const {
    const auto& plan = clause.getExecutionPlan();
    if (plan == nullptr) {
        return {};
    }

    // check if there's a plan for the current version
    auto orders = plan->getOrders();
    if (!contains(orders, version)) {
        return {};
    }

    // get the imposed order, and change it to start at zero
    const auto& order = orders.at(version);
    std::vector<unsigned int> newOrder(order->getOrder().size());
    std::transform(order->getOrder().begin(), order->getOrder().end(), newOrder.begin(),
            [](unsigned int i) -> unsigned int { return i - 1; });

    std::vector<ast::Atom*> atoms = ast::getBodyLiterals<ast::Atom>(clause);
    return reorderAtoms(atoms, newOrder);
}

int ClauseTranslator::addOperatorLevel(const ast::Node* node) {
    int nodeLevel = operators.size() + generators.size();
    operators.push_back(node);
    return nodeLevel;
}

int ClauseTranslator::addGeneratorLevel(const ast::Argument* arg) {
    int generatorLevel = operators.size() + generators.size();
    generators.push_back(arg);
    return generatorLevel;
}

void ClauseTranslator::indexNodeArguments(int nodeLevel, const std::vector<ast::Argument*>& nodeArgs) {
    for (size_t i = 0; i < nodeArgs.size(); i++) {
        const auto& arg = nodeArgs.at(i);

        // check for variable references
        if (const auto* var = dynamic_cast<const ast::Variable*>(arg)) {
            valueIndex->addVarReference(*var, nodeLevel, i);
        }

        // check for nested records
        if (auto rec = dynamic_cast<const ast::RecordInit*>(arg)) {
            valueIndex->setRecordDefinition(*rec, nodeLevel, i);

            // introduce new nesting level for unpack
            auto unpackLevel = addOperatorLevel(rec);
            indexNodeArguments(unpackLevel, rec->getArguments());
        }
    }
}

void ClauseTranslator::indexGenerator(const ast::Argument& arg) {
    int aggLoc = addGeneratorLevel(&arg);
    valueIndex->setGeneratorLoc(arg, Location({aggLoc, 0}));
}

void ClauseTranslator::indexAtoms(const ast::Clause& clause) {
    for (const auto* atom : ast::getBodyLiterals<ast::Atom>(clause)) {
        // give the atom the current level
        int scanLevel = addOperatorLevel(atom);
        indexNodeArguments(scanLevel, atom->getArguments());
    }
}

void ClauseTranslator::indexAggregatorBody(const ast::Aggregator& agg) {
    auto aggLoc = valueIndex->getGeneratorLoc(agg);

    // Get the single body atom inside the aggregator
    const auto& aggBodyAtoms =
            filter(agg.getBodyLiterals(), [&](const ast::Literal* lit) { return isA<ast::Atom>(lit); });
    assert(aggBodyAtoms.size() == 1 && "exactly one atom should exist per aggregator body");
    const auto* aggAtom = static_cast<const ast::Atom*>(aggBodyAtoms.at(0));

    // Add the variable references inside this atom
    const auto& aggAtomArgs = aggAtom->getArguments();
    for (size_t i = 0; i < aggAtomArgs.size(); i++) {
        const auto* arg = aggAtomArgs.at(i);
        if (const auto* var = dynamic_cast<const ast::Variable*>(arg)) {
            valueIndex->addVarReference(*var, aggLoc.identifier, (int)i);
        }
    }
}

void ClauseTranslator::indexAggregators(const ast::Clause& clause) {
    // Add each aggregator as an internal generator
    visitDepthFirst(clause, [&](const ast::Aggregator& agg) { indexGenerator(agg); });

    // Index aggregator bodies
    visitDepthFirst(clause, [&](const ast::Aggregator& agg) { indexAggregatorBody(agg); });

    // Add aggregator value introductions
    visitDepthFirst(clause, [&](const ast::BinaryConstraint& bc) {
        if (!isEqConstraint(bc.getBaseOperator())) return;
        const auto* lhs = dynamic_cast<const ast::Variable*>(bc.getLHS());
        const auto* rhs = dynamic_cast<const ast::Aggregator*>(bc.getRHS());
        if (lhs == nullptr || rhs == nullptr) return;
        valueIndex->addVarReference(*lhs, valueIndex->getGeneratorLoc(*rhs));
    });
}

void ClauseTranslator::indexMultiResultFunctors(const ast::Clause& clause) {
    // Add each multi-result functor as an internal generator
    visitDepthFirst(clause, [&](const ast::IntrinsicFunctor& func) {
        if (ast::analysis::FunctorAnalysis::isMultiResult(func)) {
            indexGenerator(func);
        }
    });

    // Add multi-result functor value introductions
    visitDepthFirst(clause, [&](const ast::BinaryConstraint& bc) {
        if (!isEqConstraint(bc.getBaseOperator())) return;
        const auto* lhs = dynamic_cast<const ast::Variable*>(bc.getLHS());
        const auto* rhs = dynamic_cast<const ast::IntrinsicFunctor*>(bc.getRHS());
        if (lhs == nullptr || rhs == nullptr) return;
        if (!ast::analysis::FunctorAnalysis::isMultiResult(*rhs)) return;
        valueIndex->addVarReference(*lhs, valueIndex->getGeneratorLoc(*rhs));
    });
}

void ClauseTranslator::indexClause(const ast::Clause& clause) {
    indexAtoms(clause);
    indexAggregators(clause);
    indexMultiResultFunctors(clause);
}

}  // namespace souffle::ast2ram
