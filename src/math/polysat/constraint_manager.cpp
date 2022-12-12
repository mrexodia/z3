/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat constraint manager

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-06

--*/

#include "math/polysat/constraint_manager.h"
#include "math/polysat/clause.h"
#include "math/polysat/solver.h"
#include "math/polysat/log.h"
#include "math/polysat/log_helper.h"
#include "math/polysat/ule_constraint.h"
#include "math/polysat/umul_ovfl_constraint.h"
#include "math/polysat/smul_fl_constraint.h"
#include "math/polysat/op_constraint.h"

namespace polysat {

    constraint_manager::constraint_manager(solver& s): s(s) {}

    void constraint_manager::assign_bv2c(sat::bool_var bv, constraint* c) {
        SASSERT_EQ(get_bv2c(bv), nullptr);
        SASSERT(!c->has_bvar());
        c->m_bvar = bv;
        m_bv2constraint.setx(bv, c, nullptr);
    }

    void constraint_manager::erase_bv2c(constraint* c) {
        SASSERT(c->has_bvar());
        SASSERT_EQ(get_bv2c(c->bvar()), c);
        m_bv2constraint[c->bvar()] = nullptr;
        c->m_bvar = sat::null_bool_var;
    }

    constraint* constraint_manager::get_bv2c(sat::bool_var bv) const {
        return m_bv2constraint.get(bv, nullptr);
    }

    void constraint_manager::ensure_bvar(constraint* c) {
        if (!c->has_bvar())
            assign_bv2c(s.m_bvars.new_var(), c);
    }

    void constraint_manager::erase_bvar(constraint* c) {
        if (c->has_bvar())
            erase_bv2c(c);
    }

    /** Add constraint to per-level storage */
    void constraint_manager::store(constraint* c) {
        LOG_V("Store constraint: " << show_deref(c));
        m_constraints.push_back(c);
    }


    void constraint_manager::register_clause(clause* cl) {
        unsigned clause_level = s.base_level();
        clause_level = 0;  // TODO: s.base_level() may be too high, if the clause is propagated at an earlier level. For now just use 0.
        while (m_clauses.size() <= clause_level)
            m_clauses.push_back({});
        m_clauses[clause_level].push_back(cl);
    }

    void constraint_manager::store(clause* cl, bool value_propagate) {
        register_clause(cl);
        watch(*cl, value_propagate);
    }

    // Release constraints at the given level and above.
    void constraint_manager::release_level(unsigned lvl) {
        for (unsigned l = m_clauses.size(); l-- > lvl; ) {
            for (auto& cl : m_clauses[l]) {
                unwatch(*cl);
                SASSERT_EQ(cl->m_ref_count, 1);  // otherwise there is a leftover reference somewhere
            }
            m_clauses[l].reset();
        }
    }

    /**
     * Move literals to be watched to the front of the clause.
     */
    void constraint_manager::normalize_watch(clause& cl) {
        SASSERT(cl.size() > 1);

        // A literal may be watched if there is no unwatched literal at higher level,
        // where true and unassigned literals are considered at infinite level.
        // We prefer true literals to unassigned literals.
        auto get_watch_level = [&](sat::literal lit) -> unsigned {
            switch (s.m_bvars.value(lit)) {
                case l_false:
                    return s.m_bvars.level(lit);
                case l_true:
                    return UINT_MAX;
                case l_undef:
                    return UINT_MAX - 1;
            }
            UNREACHABLE();
            return 0;
        };

        unsigned lvl0 = get_watch_level(cl[0]);
        unsigned lvl1 = get_watch_level(cl[1]);
        if (lvl0 < lvl1) {
            std::swap(lvl0, lvl1);
            std::swap(cl[0], cl[1]);
        }
        SASSERT(lvl0 >= lvl1);
        for (unsigned i = 2; i < cl.size(); ++i) {
            sat::literal const lit = cl[i];
            unsigned const lvl = get_watch_level(lit);
            if (lvl > lvl0) {
                cl[i] = cl[1];
                cl[1] = cl[0];
                lvl1 = lvl0;
                cl[0] = lit;
                lvl0 = lvl;
            }
            else if (lvl > lvl1) {
                cl[i] = cl[1];
                cl[1] = lit;
                lvl1 = lvl;
            }
            SASSERT_EQ(lvl0, get_watch_level(cl[0]));
            SASSERT_EQ(lvl1, get_watch_level(cl[1]));
            SASSERT(lvl0 >= lvl1 && lvl1 >= get_watch_level(cl[i]));
        }
        SASSERT(all_of(cl, [&](auto lit) { return get_watch_level(lit) <= get_watch_level(cl[0]); }));
        SASSERT(std::all_of(cl.begin() + 1, cl.end(), [&](auto lit) { return get_watch_level(lit) <= get_watch_level(cl[1]); }));
    }

    void constraint_manager::watch(clause& cl, bool value_propagate) {
        if (cl.empty())
            return;

        if (value_propagate) {
#if 1
            // First, try to bool-propagate.
            // Otherwise, we might get a clause-conflict and a missed propagation after resolving the conflict.
            // With this, we will get a constraint-conflict instead.
            // TODO: maybe it makes sense to choose bool vs. eval depending on which has the lower level?
            sat::literal undef_lit = sat::null_literal;
            for (sat::literal lit : cl) {
                if (s.m_bvars.is_false(lit))
                    continue;
                if (s.m_bvars.is_true(lit)) {
                    undef_lit = sat::null_literal;
                    break;
                }
                SASSERT(!s.m_bvars.is_assigned(lit));
                if (undef_lit == sat::null_literal)
                    undef_lit = lit;
                else {
                    undef_lit = sat::null_literal;
                    break;
                }
            }
            if (undef_lit != sat::null_literal)
                s.assign_propagate(undef_lit, cl);

            // this should be already done with insert_eval when constructing the clause (maybe not for non-redundant clauses?)
            // (this loop also masks the mistake of calling clause_builder::insert instead of clause_builder::insert_eval)
            for (sat::literal lit : cl) {
                if (s.m_bvars.is_false(lit))
                    continue;
                signed_constraint sc = s.lit2cnstr(lit);
                if (sc.is_currently_false(s)) {
                    if (s.m_bvars.is_true(lit)) {
                        s.set_conflict(sc);
                        return;
                    }
                    s.assign_eval(~lit);
                }
            }
#endif
        }

        if (cl.size() == 1) {
            if (s.m_bvars.is_false(cl[0]))
                s.set_conflict(cl);
            else if (!s.m_bvars.is_assigned(cl[0]))
                s.assign_propagate(cl[0], cl);
            return;
        }

        normalize_watch(cl);

        s.m_bvars.watch(cl[0]).push_back(&cl);
        s.m_bvars.watch(cl[1]).push_back(&cl);

        if (s.m_bvars.is_true(cl[0]))
            return;
        SASSERT(!s.m_bvars.is_true(cl[1]));
        if (!s.m_bvars.is_false(cl[1])) {
            SASSERT(!s.m_bvars.is_assigned(cl[0]) && !s.m_bvars.is_assigned(cl[1]));
            return;
        }
        if (s.m_bvars.is_false(cl[0]))
            s.set_conflict(cl);
        else
            s.assign_propagate(cl[0], cl);
    }

    void constraint_manager::unwatch(clause& cl) {
        if (cl.size() <= 1)
            return;
        s.m_bvars.watch(~cl[0]).erase(&cl);
        s.m_bvars.watch(~cl[1]).erase(&cl);
    }

    constraint_manager::~constraint_manager() {
        // Release explicitly to check for leftover references in debug mode,
        // and to make sure all constraints are destructed before the bvar->constraint mapping.
        release_level(0);
    }

    constraint* constraint_manager::lookup(sat::bool_var var) const {
        return get_bv2c(var);
    }

    signed_constraint constraint_manager::lookup(sat::literal lit) const {
        return {lookup(lit.var()), lit};
    }

    /** Look up constraint among stored constraints. */
    constraint* constraint_manager::dedup(constraint* c1) {
        constraint* c2 = nullptr;
        if (m_dedup.constraints.find(c1, c2)) {
            dealloc(c1);
            return c2;
        }
        else {
            SASSERT(!c1->has_bvar());
            ensure_bvar(c1);
            m_dedup.constraints.insert(c1);
            store(c1);
            return c1;
        }
    }

    void constraint_manager::gc() {
        LOG_H1("gc");
        gc_clauses();
        gc_constraints();
    }

    void constraint_manager::gc_clauses() {
        LOG_H3("gc_clauses");
        // place to gc redundant clauses
    }

    void constraint_manager::gc_constraints() {
        LOG_H3("gc_constraints");
        uint_set used_vars;
        for (auto const& cls : m_clauses)
            for (auto const& cl : cls)
                for (auto lit : *cl)
                    used_vars.insert(lit.var());
        // anything on the search stack is justified by a clause?
        for (auto const& a : s.m_search)
            if (a.is_boolean())
                used_vars.insert(a.lit().var());
        for (unsigned i = 0; i < m_constraints.size(); ++i) {
            constraint* c = m_constraints[i];
            if (c->has_bvar() && used_vars.contains(c->bvar()))
                continue;
            if (c->is_external())
                continue;
            LOG("Erasing: " << show_deref(c));
            erase_bvar(c);
            m_constraints.swap(i, m_constraints.size() - 1);
            m_constraints.pop_back();
            --i;
        }

    }

    bool constraint_manager::should_gc() {
        return false;
        // TODO control gc decay rate
        return m_constraints.size() > m_num_external + 100;
    }

    signed_constraint constraint_manager::ule(pdd const& a, pdd const& b) {
        bool is_positive = true;
        pdd lhs = a;
        pdd rhs = b;
        ule_constraint::simplify(is_positive, lhs, rhs);
        return { dedup(alloc(ule_constraint, *this, lhs, rhs)), is_positive };
    }

    signed_constraint constraint_manager::eq(pdd const& p) {
        return ule(p, p.manager().zero());
    }

    signed_constraint constraint_manager::ult(pdd const& a, pdd const& b) {
        return ~ule(b, a);
    }

    /**
    * encode that the i'th bit of p is 1.
    * It holds if p << (K - i - 1) >= 2^{K-1}, where K is the bit-width.
    */
    signed_constraint constraint_manager::bit(pdd const& p, unsigned i) {
        unsigned K = p.manager().power_of_2();
        pdd q = p * rational::power_of_two(K - i - 1);
        rational msb = rational::power_of_two(K - 1);
        return ule(p.manager().mk_val(msb), q);
    }

    signed_constraint constraint_manager::umul_ovfl(pdd const& a, pdd const& b) {
        return { dedup(alloc(umul_ovfl_constraint, *this, a, b)), true };
    }

    signed_constraint constraint_manager::smul_ovfl(pdd const& a, pdd const& b) {
        return { dedup(alloc(smul_fl_constraint, *this, a, b, true)), true };
    }

    signed_constraint constraint_manager::smul_udfl(pdd const& a, pdd const& b) {
        return { dedup(alloc(smul_fl_constraint, *this, a, b, false)), true };
    }

    signed_constraint constraint_manager::mk_op_constraint(op_constraint::code op, pdd const& p, pdd const& q, pdd const& r) {
        return { dedup(alloc(op_constraint, *this, op, p, q, r)), true };
    }

    // To do signed comparison of bitvectors, flip the msb and do unsigned comparison:
    //
    //      x <=s y    <=>    x + 2^(w-1)  <=u  y + 2^(w-1)
    //
    // Example for bit width 3:
    //      111  -1
    //      110  -2
    //      101  -3
    //      100  -4
    //      011   3
    //      010   2
    //      001   1
    //      000   0
    //
    // Argument: flipping the msb swaps the negative and non-negative blocks
    //
    signed_constraint constraint_manager::sle(pdd const& a, pdd const& b) {
        auto shift = rational::power_of_two(a.power_of_2() - 1);
        return ule(a + shift, b + shift);
    }

    signed_constraint constraint_manager::slt(pdd const& a, pdd const& b) {
        auto shift = rational::power_of_two(a.power_of_2() - 1);
        return ult(a + shift, b + shift);
    }

    /** unsigned quotient/remainder */
    std::pair<pdd, pdd> constraint_manager::quot_rem(pdd const& a, pdd const& b) {
        auto& m = a.manager();
        unsigned sz = m.power_of_2();
        if (b.is_zero()) {
            // By SMT-LIB specification, b = 0 ==> q = -1, r = a.
            return {m.mk_val(m.max_value()), a};
        }
        if (b.is_one()) {
            return {a, m.zero()};
        }
        if (a.is_val() && b.is_val()) {
            rational const av = a.val();
            rational const bv = b.val();
            SASSERT(!bv.is_zero());
            rational rv;
            rational qv = machine_div_rem(av, bv, rv);
            pdd q = m.mk_val(qv);
            pdd r = m.mk_val(rv);
            SASSERT_EQ(a, b * q + r);
            SASSERT(b.val()*q.val() + r.val() <= m.max_value());
            SASSERT(r.val() <= (b*q+r).val());
            SASSERT(r.val() < b.val());
            return {std::move(q), std::move(r)};
        }
#if 0
        unsigned pow;
        if (b.is_val() && b.val().is_power_of_two(pow)) {
            // TODO: q = a >> b.val()
            //       r = 0 \circ a[pow:]  ???
        }
#endif
        constraint_dedup::quot_rem_args args({a, b});
        auto it = m_dedup.quot_rem_expr.find_iterator(args);
        if (it != m_dedup.quot_rem_expr.end())
            return { m.mk_var(it->m_value.first), m.mk_var(it->m_value.second) };

        pdd q = m.mk_var(s.add_var(sz));  // quotient
        pdd r = m.mk_var(s.add_var(sz));  // remainder
        m_dedup.quot_rem_expr.insert(args, { q.var(), r.var() });

        // Axioms for quotient/remainder:
        //      a = b*q + r
        //      multiplication does not overflow in b*q
        //      addition does not overflow in (b*q) + r; for now expressed as: r <= bq+r
        //      b ≠ 0  ==>  r < b
        //      b = 0  ==>  q = -1
        s.add_clause(eq(b * q + r - a), false);
        s.add_clause(~umul_ovfl(b, q), false);
        // r <= b*q+r
        //  { apply equivalence:  p <= q  <=>  q-p <= -p-1 }
        // b*q <= -r-1
        s.add_clause(ule(b*q, -r-1), false);
#if 0
        // b*q <= b*q+r
        //  { apply equivalence:  p <= q  <=>  q-p <= -p-1 }
        // r <= - b*q - 1
        s.add_clause(ule(r, -b*q-1), false);  // redundant, but may help propagation
#endif

        auto c_eq = eq(b);
        s.add_clause(c_eq, ult(r, b), false);
        s.add_clause(~c_eq, eq(q + 1), false);

        return {std::move(q), std::move(r)};
    }

    pdd constraint_manager::bnot(pdd const& p) {
        return -p - 1;
    }

    pdd constraint_manager::mk_op_term(op_constraint::code op, pdd const& p, pdd const& q) {
        auto& m = p.manager();
        unsigned sz = m.power_of_2();

        op_constraint_args const args(op, p, q);
        auto it = m_dedup.op_constraint_expr.find_iterator(args);
        if (it != m_dedup.op_constraint_expr.end())
            return m.mk_var(it->m_value);

        pdd r = m.mk_var(s.add_var(sz));
        m_dedup.op_constraint_expr.insert(args, r.var());

        s.add_clause(mk_op_constraint(op, p, q, r), false);
        return r;
    }

    pdd constraint_manager::lshr(pdd const& p, pdd const& q) {
        return mk_op_term(op_constraint::code::lshr_op, p, q);
    }

    pdd constraint_manager::shl(pdd const& p, pdd const& q) {
        return mk_op_term(op_constraint::code::shl_op, p, q);
    }

    pdd constraint_manager::band(pdd const& p, pdd const& q) {
        return mk_op_term(op_constraint::code::and_op, p, q);
    }

    pdd constraint_manager::bor(pdd const& p, pdd const& q) {
        // From "Hacker's Delight", section 2-2. Addition Combined with Logical Operations;
        // found via Int-Blasting paper; see https://doi.org/10.1007/978-3-030-94583-1_24
        return (p + q) - band(p, q);
    }

    pdd constraint_manager::bxor(pdd const& p, pdd const& q) {
        // From "Hacker's Delight", section 2-2. Addition Combined with Logical Operations;
        // found via Int-Blasting paper; see https://doi.org/10.1007/978-3-030-94583-1_24
        return (p + q) - 2*band(p, q);
    }

    pdd constraint_manager::bnand(pdd const& p, pdd const& q) {
        return bnot(band(p, q));
    }

    pdd constraint_manager::bnor(pdd const& p, pdd const& q) {
        return bnot(bor(p, q));
    }
}