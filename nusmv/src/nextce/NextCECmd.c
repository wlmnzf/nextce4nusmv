#include <stdio.h>

#include "NextCECmd.h"
#include "NextCEDbg.h"
#include "prop/Prop.h"
#include "prop/PropDb.h"
#include "prop/propPkg.h"
#include "fsm/sexp/Expr.h"
#include "trace/Trace.h"
#include "trace/TraceManager.h"
#include "trace/pkg_trace.h"
#include "trace/plugins/TraceExplainer.h"
#include "parser/symbols.h"
#include "node/node.h"
#include "ltl/ltl.h"

/* Used in debug/error printouts */
extern FILE * nusmv_stderr;
/**
 * This structure contains the options passed as command line arguments.
 */
typedef struct {
	int prop_num;
} options_t;

/* Forward declarations: */
static Expr_ptr generate_state_eq(Trace_ptr trace, TraceIter iter);
static Expr_ptr get_inv(Prop_ptr prop);

/**
 * Debug function to display in a human-readable way the command line options
 * as they were understood
 * @param fname The name of the calling function
 * @param options The command line arguments structure to display
 */
void debug_show_options(const char * fname, const options_t *options) {
	nextce_debug(5, "%s: Showing options:", fname);
	nextce_debug(5, "%s: \tprop_num: %d", fname, options->prop_num);
}

/**
 * Returns the counter example (as a trace/Trace_ptr) of the given formula (as a
 * propery/Prop_ptr).
 * @param prop The property whose trace to reture
 * @returns the trace (counter example) of the given property
 */
static Trace_ptr get_ce(Prop_ptr prop) {
	static const char * fname = __func__;
	int index = Prop_get_trace(prop);
	nextce_debug(5, "%s: Enter. Index: %d", fname, index);
	TraceManager_ptr traceManager = TracePkg_get_global_trace_manager();
	Trace_ptr trace = TraceManager_get_trace_at_index(traceManager, index-1);
	nextce_debug(5, "%s: Exit", fname);
	return trace;
}

/**
 * Return a conjunction of the two given parameters.
 * @param dest The first expression/parameter
 * @param src The second expression/parameter
 * @return The expression: dest AND src
 */
static Expr_ptr conjunct(node_ptr dest, node_ptr src) {
	if (dest) {
		return Expr_and(dest, src);
	}
	return src;
}

/**
 * Create a new FlatHierarchy object for the property - specifically for this
 * specific step in the counter example.
 * This structure is necessary to construct a FSM that will state whether this
 * state holds for a given LTL specification
 * @param trace The counter example
 * @param step The specific step within the counter example
 * @return A new FlatHierarchy_ptr object
 */
static FlatHierarchy_ptr create_new_prop_flat_hierarchy(
		Trace_ptr trace, TraceIter step) {
	Expr_ptr init = NULL;
	Expr_ptr trans = NULL;
	node_ptr symbol = NULL;
	TraceSymbolsIter symbols_iter;
	SymbTable_ptr st;
	SymbTable_ptr newSt;
	SymbLayer_ptr layer;
	FlatHierarchy_ptr fh;

	st = Trace_get_symb_table(trace);
	newSt = SymbTable_create();
	layer = SymbTable_create_layer(newSt, NULL, SYMB_LAYER_POS_BOTTOM);
	fh = FlatHierarchy_create(newSt);
	TRACE_SYMBOLS_FOREACH(trace, TRACE_ITER_ALL_VARS, symbols_iter, symbol) {
		SymbType_ptr type = SymbTable_get_var_type(st, symbol);
		SymbLayer_declare_state_var(layer, symbol, type);
		node_ptr value = Trace_step_get_value(trace, step, symbol);
		Expr_ptr initAss = Expr_equal(symbol, value, newSt);
		Expr_ptr transAss = Expr_equal(Expr_next(symbol, newSt), value, newSt);
		FlatHierarchy_add_var(fh, symbol);
		init = conjunct(init, initAss);
		trans = conjunct(trans, transAss);
	}
	FlatHierarchy_set_init(fh, init);
	FlatHierarchy_set_trans(fh, trans);
	return fh;
}

/* In the following function, these debug messages can be used:
 * To debug sexpfsm:
	printf("Scalar expression FSM:\n");
	printf("Init:\n");
	print_node(stdout, SexpFsm_get_init(sexpfsm));
	printf("\n");
	printf("Invar:\n");
	print_node(stdout, SexpFsm_get_invar(sexpfsm));
	printf("\n");
	printf("Trans:\n");
	print_node(stdout, SexpFsm_get_trans(sexpfsm));
	printf("\n");
	printf("Input:\n");
	print_node(stdout, SexpFsm_get_input(sexpfsm));
	printf("\n");
 * To debug new_fsm:
	BddFsm_print_reachable_states_info(new_fsm, true, true, false, stdout);
	BddFsm_print_reachable_states_info(new_fsm, false, true, true, stdout);
 */
/* Used to construct FSM (finite state machines) */
EXTERN FsmBuilder_ptr global_fsm_builder;
/**
 * Create a new property to test whether a specific step in the given counter-
 * example (that belongs to the given property) holds on a given expression.
 * The property is also set with the generated FSMs.
 * This method is used to find the first step in the counter-example that does
 * not hold for the LTL expression.
 * @param prop The original property
 * @param expr The LTL expression we want to test
 * @param trace The counter-example we are looking
 * @param step The current step in the counter-example we suspect as not holding
 * 		for the given expression (expr)
 * @return A new property that when verified, will return true if the step holds
 * 		for the LTL expression.
 */
static Prop_ptr get_new_prop_to_test(Prop_ptr prop, Expr_ptr expr, Trace_ptr trace, TraceIter step) {
	SexpFsm_ptr sexpfsm;
	BddFsm_ptr prop_bdd_fsm;
	BddFsm_ptr new_fsm;
	TransType  trans_type;
	BddEnc_ptr bdd_enc;
	Prop_ptr res;
	FlatHierarchy_ptr fh;

	fh = create_new_prop_flat_hierarchy(trace, step);
	sexpfsm = SexpFsm_create(fh, FlatHierarchy_get_vars(fh));
	FlatHierarchy_destroy(fh);

	/* Obtain the BddFsm of the original property */
	prop_bdd_fsm = Prop_get_bdd_fsm(prop);

	/* get the needed encoders */
	bdd_enc = BddFsm_get_bdd_encoding(prop_bdd_fsm);

	trans_type = GenericTrans_get_type(GENERIC_TRANS(BddFsm_get_trans(prop_bdd_fsm)));
	new_fsm = FsmBuilder_create_bdd_fsm(global_fsm_builder, bdd_enc, sexpfsm, trans_type);
	res = Prop_create_partial(expr, Prop_Ltl);
	Prop_set_bdd_fsm(res, new_fsm);
	Prop_set_scalar_sexp_fsm(res, sexpfsm);
	SexpFsm_destroy(sexpfsm);
	return res;
}

/**
 * Test if the given step (step) in the given counter example (trace) of the
 * given property (prop) holds under the given expression (expr).
 * This method is used to find the first step in the counter-example that does
 * not hold for the LTL expression.
 * @param prop The property for which the counter example is given
 * @param expr The counter example step should hold for this expression
 * @param trace The counter example
 * @param step The step in the counter example - we want to know if this step
 * 		holds for expr.
 * @return True (or a C equivalent) if step holds for expr. False otherwise.
 */
static int test_expr_in_state(Prop_ptr prop, Expr_ptr expr, Trace_ptr trace, TraceIter step) {
	Prop_ptr test = get_new_prop_to_test(prop, expr, trace, step);
	// Prop_verify(test);
	Ltl_CheckLtlSpecSilent(test);
	return (Prop_get_status(test) == Prop_True);
}

/**
 * Find the first step in the given counter example (trace) which does not hold
 * for the expression in the property. i.e. find the first step in the counter
 * example where the property fails the expression
 * @param prop The property that generated the counter example (also contains the expr.)
 * @param trace The counter example
 * @return An iterator pointing to the first step failing the expression
 */
static TraceIter find_trunc_trace_step(Prop_ptr prop, Trace_ptr trace) {
	static const char * fname = __func__;
	TraceIter first = NULL;
	TraceIter step;
	Expr_ptr inv = get_inv(prop);
	int cnt = 1;

	TRACE_FOREACH(trace, step) {
		if (!test_expr_in_state(prop, inv, trace, step)) {
			first = step;
			break;
		}
		++cnt;
	}
	nextce_debug(5, "%s: truncating from step (exclusive): %d", fname, cnt);
	return first;
}

/**
 * Create an expression that tests whether the current state is an initial state -
 * i.e. can the FSM start from this state?
 * This is done, since we want to trim counter examples to be as short as possible.
 * We want to select the latest step that can be considered an initial step, and
 * ignore what happened before it (since a shorter counter example can be generated
 * by starting directly from this step).
 * @param trace The counter example
 * @param sexpfsm The FSM
 * @return The expression to test whether a step is an initial state in sexpfsm
 */
static Expr_ptr construct_is_init_state_expr(
		Trace_ptr trace, SexpFsm_ptr sexpfsm) {
	static const char * fname = __func__;
	TraceSymbolsIter symbols_iter;
	node_ptr symbol = NULL;
	Expr_ptr result = NULL;
	SymbTable_ptr symb_table = Trace_get_symb_table(trace);

	TRACE_SYMBOLS_FOREACH(trace, TRACE_ITER_ALL_VARS, symbols_iter, symbol) {
		node_ptr value = SexpFsm_get_var_init(sexpfsm, symbol);
		Expr_ptr symb_val_pair;
		if (!value) {
			nextce_debug(5, "%s: Did not find symbol:", fname);
			if (is_nextce_debug(5)) {
				print_node(nusmv_stderr, symbol);
			}
			nextce_debug(5, "");
			continue;
		}
		if (Expr_get_type(value) == EQUAL) {
			nextce_debug(5, "%s: Expr type is equals", fname);
		}
		value = cdr(value);		
		symb_val_pair = Expr_setin(symbol, value, symb_table);
		if (result) {
			result = Expr_and(result, symb_val_pair);
		} else {
			result = symb_val_pair;
		}
	}
	if (is_nextce_debug(5)) {
		printf("%s: Exiting with:", fname);
		if (result) {
			print_node(stdout, result);
		} else {
			printf("Nothing");
		}
		printf("\n");
	}
	return result;
	
}

/**
 * Given a counter example, find the last step (before a given 'end step') that
 * comes can be considered an initial step (i.e. the counter example could have
 * started from it, and we can trim the counter example up to that point).
 * @param prop The property that generated the counter example
 * @param trace The counter example
 * @param until_here Consider steps only up to (and not including) this step
 * @return An iterator pointing to the latest relevant initial state
 */
static TraceIter find_init_trace_step(
		Prop_ptr prop, Trace_ptr trace, TraceIter until_here) {
	static const char * fname = __func__;
	SexpFsm_ptr sexpfsm = Prop_get_scalar_sexp_fsm(prop);
	Expr_ptr expr;
	TraceIter last = NULL;
	TraceIter step;
	int cnt = 1;
	int last_cnt = 0;

	expr = construct_is_init_state_expr(trace, sexpfsm);
	if (is_nextce_debug(5)) {
		printf("%s: Testing against this expression:\n", fname);
		print_node(stdout, expr);
		printf("\n");
	}
	TRACE_FOREACH(trace, step) {
		if (step == until_here) {
			break;
		}
		if (test_expr_in_state(prop, expr, trace, step)) {
			last = step;
			last_cnt = cnt;
		}
		++cnt;
	}
	nextce_debug(5, "%s: trimming up to step (exclusive): %d", fname, last_cnt);
	return last;
}

/**
 * Create a FIPATH from a counter example. FIPATHs are defined in the paper.
 * It appears that NuSMV takes care of the following steps for us:
 * progress, reduce_config_loops, reduce_vals
 * @param prop The property that generated the counter example
 * @param trace The counter example
 * @return The FIPATH - A trimmed counter example containing less information we
 * 		do not need.
 */
static Trace_ptr create_fipath(Prop_ptr prop, Trace_ptr trace) {
	static const char * fname = __func__;
	TraceIter until_here = find_trunc_trace_step(prop, trace);
	TraceIter from_here = find_init_trace_step(prop, trace, until_here);
	Trace_ptr result = Trace_copy_ex(trace, from_here,
			until_here, FALSE);
//    Trace_ptr result =trace_copy_ex(trace, from_here,
//                  until_here, FALSE);
/*	TODO We assume these are given to us by NuSMV.
	fipath_progress(result);
	fipath_reduce_config_loops(result);
	fipath_reduce_vals(result);*/
	if (is_nextce_debug(5)) {
		static TracePlugin_ptr plugin = NULL;
		static TraceOpt_ptr opt = NULL;
		if (!plugin) {
			plugin = TraceExplainer_create(true);
			opt = TraceOpt_create_from_env(OptsHandler_get_instance());
		}
		printf("Created this fipath:\n");
		TracePlugin_action(plugin, result, opt);
	}
	return result;
}

/**
 * A debug function to print the given expression
 * @param expr The expression to print
 */
static void debug_print_expr(Expr_ptr expr) {
	Prop_ptr prop;
	if (!is_nextce_debug(5)) {
		return;
	}
	prop = Prop_create_partial(expr, Prop_Ltl);
	Prop_print(prop, stdout, PROP_PRINT_FMT_FORMULA);
	fprintf(stdout, "\n");
	Prop_destroy(prop);
}

/* Create \phi U \psi */
static Expr_ptr nextce_expr_until(Expr_ptr phi, Expr_ptr psi) {
	Expr_ptr result = new_node(UNTIL, phi, psi);
	return result;
}

/* Create X \psi */
static Expr_ptr nextce_expr_next(Expr_ptr psi) {
	Expr_ptr result = new_node(OP_NEXT, psi, Nil);
	return result;
}

/* Create Y \psi */
static Expr_ptr nextce_expr_prev(Expr_ptr psi) {
	Expr_ptr result = new_node(OP_PREC, psi, Nil);
	return result;
}

/**
 * Extract the invariant from the given property. We assume the property's
 * formula is given as an LTLSPEC: G(inv). This method returns inv.
 * @param prop The property defining the expression
 * @return The invariant expression defined in the property
 */
static Expr_ptr get_inv(Prop_ptr prop) {
	Expr_ptr expr = Prop_get_expr(prop);
	int type = node_get_type(expr);
	switch (type) {
		case CONTEXT:
			return cadr(expr);
		case OP_GLOBAL:
			return car(expr);
		default:
			fprintf(stderr, "Unkown node type %d: ", type);
			print_node(nusmv_stderr, expr);
			fprintf(stderr, "\n");
	}
	return expr; /* Unhealthy default */
}

/**
 * Generate an expression describing the given step in the counter example.
 * Basically creates an expression V (variable == value) where V is the conjunction
 * operator, iterating all variables in the counter example step.
 * @param trace The counter example
 * @param iter The step in the counter example
 * @return An expression for the given step in the given counter example
 */
static Expr_ptr generate_state_eq(Trace_ptr trace, TraceIter iter) {
	static const char * fname = __func__;
	TraceSymbolsIter symbols_iter;
	node_ptr symbol = NULL;
	Expr_ptr result = NULL;
	SymbTable_ptr symb_table = Trace_get_symb_table(trace);

	TRACE_SYMBOLS_FOREACH(trace, TRACE_ITER_ALL_VARS, symbols_iter, symbol) {
		node_ptr value = Trace_step_get_value(trace, iter, symbol);
		Expr_ptr symb_val_pair;
		if (!value) {
			nextce_debug(5, "%s: Did not find symbol:", fname);
			if (is_nextce_debug(5)) {
				print_node(nusmv_stderr, symbol);
			}
			nextce_debug(5, "");
			continue;
		}
		symb_val_pair = Expr_equal(symbol, value, symb_table);
		if (result) {
			result = Expr_and(result, symb_val_pair);
		} else {
			result = symb_val_pair;
		}
	}
	if (is_nextce_debug(5)) {
		printf("%s: Exiting with:", fname);
		if (result) {
			print_node(stdout, result);
		} else {
			printf("Nothing");
		}
		printf("\n");
	}
	return result;
}

/**
 * Generate a disjunct for the 1st equivalence class
 * @param prop The property for which to generate the disjunct
 * @param fipath The FIPATH on which to base the disjunct.
 * @return A disjunct for the 1st equivalence class
 */
static Expr_ptr generate_disjunc_1(Prop_ptr prop, Trace_ptr fipath) {
	static const char * fname = __func__;
	TraceIter state;
	Expr_ptr inv = NULL;
	Expr_ptr expr = NULL;
	Expr_ptr result = NULL;
	nextce_debug(5, "%s: Enter", fname); 

	inv = get_inv(prop);
	state = Trace_last_iter(fipath);
	expr = generate_state_eq(fipath, state);
	result = Expr_and(Expr_not(inv), expr);
	state = TraceIter_get_prev(state);
	while (state != NULL)
	{
		expr = generate_state_eq(fipath, state);
		result = Expr_and(expr, nextce_expr_until(inv, result));
		state = TraceIter_get_prev(state);
	}
	return result;
}

/**
 * Generate a disjunct for the 2nd equivalence class
 * @param prop The property for which to generate the disjunct
 * @param fipath The FIPATH on which to base the disjunct.
 * @return A disjunct for the 2nd equivalence class
 */
static Expr_ptr generate_disjunc_2(Prop_ptr prop, Trace_ptr fipath) {
	static const char * fname = __func__;
	TraceIter state;
	Expr_ptr inv = NULL;
	Expr_ptr last = NULL;
	Expr_ptr before_last = NULL;
	Expr_ptr result = NULL;
	SymbTable_ptr symb_table = Trace_get_symb_table(fipath);

	nextce_debug(5, "%s: Enter", fname);
	state = Trace_last_iter(fipath);
	last = generate_state_eq(fipath, state);	
	state = TraceIter_get_prev(state);
	before_last = generate_state_eq(fipath, state);
	inv = get_inv(prop);
	result = Expr_and(nextce_expr_prev(before_last), last);
	result = Expr_and(Expr_not(inv), result);
	result = nextce_expr_until(inv, result);
	return result;
}

static Expr_ptr generate_disjunc_4(Prop_ptr prop, Trace_ptr fipath);
/**
 * Generate a disjunct for the 3rd equivalence class
 * @param prop The property for which to generate the disjunct
 * @param fipath The FIPATH on which to base the disjunct.
 * @return A disjunct for the 3rd equivalence class
 */
static Expr_ptr generate_disjunc_3(Prop_ptr prop, Trace_ptr fipath) {
	static const char * fname = __func__;
	TraceIter first_state;
	Expr_ptr first = NULL;
	Expr_ptr result = NULL;

	nextce_debug(5, "%s: Enter", fname);
	result = generate_disjunc_4(prop, fipath);
	first_state = Trace_first_iter(fipath);
	first = generate_state_eq(fipath, first_state);
	if (!first) { /* Should never happen */
		return result;
	}
	if (!result) { /* Should never happen */
		return first;
	}
	return Expr_and(first, result);
}

/**
 * Generate a disjunct for the 4th equivalence class
 * @param prop The property for which to generate the disjunct
 * @param fipath The FIPATH on which to base the disjunct.
 * @return A disjunct for the 4th equivalence class
 */
static Expr_ptr generate_disjunc_4(Prop_ptr prop, Trace_ptr fipath) {
	static const char * fname = __func__;
	TraceIter last_state;
	TraceSymbolsIter symbols_iter;
	Expr_ptr result = NULL;
	Expr_ptr inv = NULL;
	node_ptr symbol = NULL;
	SymbTable_ptr symb_table;

	nextce_debug(5, "%s: Enter", fname);
	inv = get_inv(prop);
	last_state = Trace_last_iter(fipath);
	result = generate_state_eq(fipath, last_state);
	result = Expr_and(Expr_not(inv), result);
	result = nextce_expr_until(inv, result);
	return result;
}

/**
 * Generate a disjunct for the given property, according to the given FIPATH.
 * This disjunct can then be disjuncted with the rest of the disjuncts and the
 * property's expression, to generate the next counter example.
 * The equivalence class is taken from the global option.
 * @param prop The property for which to generate the disjunct
 * @param fipath The FIPATH on which to base the disjunct.
 * @return A disjunct for the given property
 */
static Expr_ptr generate_disjunc(Prop_ptr prop, Trace_ptr fipath) {
	static const char * fname = __func__;
	Expr_ptr result = NULL;
	int eq_class = NextCE_get_equivalence_class();
	nextce_debug(5, "%s: Enter: eq class (%d)", fname, eq_class);
	switch (eq_class) {
		case 1:
			result = generate_disjunc_1(prop, fipath);
			break;
		case 2:
			result = generate_disjunc_2(prop, fipath);
			break;
		case 3:
			result = generate_disjunc_3(prop, fipath);
			break;
		case 4:
			result = generate_disjunc_4(prop, fipath);
			break;
	}
	nextce_debug(5, "%s: Exit", fname);
	debug_print_expr(result);
	return result;
}

/**
 * Generate a disjunct, and append it to the nextce internal structure.
 * @param prop The property for which to create the disjunct
 * @param fipath The FIPATH on which to base the disjunct
 */
void generate_and_append_disjunc(Prop_ptr prop, Trace_ptr fipath) {
	Expr_ptr lqi = generate_disjunc(prop, fipath);
	NextCE_ptr nextce = Prop_get_nextce_data(prop);
	if (!nextce) {
		nextce = NextCE_create();
		Prop_set_nextce_data(prop, nextce);
	}
	NextCE_add_disjunct(nextce, lqi);
}

/**
 * Create a new expression as a disjunction of the given property's expression
 * and the disjuncts in the nextce internal structure
 * @param prop The property for which to create the expression
 * @return an expression to generate the next counter example.
 */
static Expr_ptr create_new_expr(Prop_ptr prop) {
	static const char * fname = __func__;
	Expr_ptr result;
	Expr_ptr disjunct;
	node_ptr list;
	NextCE_ptr nextce;

	nextce_debug(5, "%s: Enter", fname); 
	result = Prop_get_expr(prop);
	nextce = Prop_get_nextce_data(prop);
	for (list = NextCE_get_disjuncts(nextce); list != Nil; list = cdr(list)) {
		disjunct = car(list);
		nextce_debug(5, "%s: Orring the disjunct:", fname); 
		debug_print_expr(disjunct);
		result = Expr_or(result, disjunct);
	}
	nextce_debug(5, "%s: Exit with:", fname); 
	debug_print_expr(result);
	return result;
}

/* Update the property - if property hasn't been checked, do nothing. Otherwise,
add L_{q_i} to the property.*/ 
/**
 * In general, create a new property. Assign to it the expression generate by
 * #create_new_expr, and return it.
 * In special cases, where there is no need to create a new expression (after
 * reset, or first run), this method may return the original property.
 * @param prop The original property.
 * @param options The command line arguments
 * @return A new property that when validated will generate the next counter-example
 */
Prop_ptr create_updated_prop(Prop_ptr prop, const options_t * options) {
	static const char * fname = __func__;
	Prop_Status status;
	NextCE_ptr nextce;
	Trace_ptr trace;
	Trace_ptr fipath;
	Expr_ptr Lqi;
	Expr_ptr newProp;
	nextce_debug(5, "%s: Enter", fname); 
	nextce = Prop_get_nextce_data(prop);
	if (nextce && (NextCE_get_status(nextce) == NextCE_Reset)) {
		nextce_debug(5, "%s: Exit: Property CE reset - starting anew", fname); 
		/* Return clone */
		return Prop_create_partial(Prop_get_expr(prop), Prop_get_type(prop));
	}
	status = Prop_get_status(prop);
	if ((status == Prop_NoStatus) || (status == Prop_Unchecked)) {
		nextce_debug(5, "%s: Exit: Propery is not checked - string anew", fname); 
		return prop;
	}
	if (nextce && (NextCE_get_status(nextce) == NextCE_True)) {
		nextce_debug(5, "%s: Exit: Propery is not false", fname); 
		fprintf(stdout, "No more counterexamples\n");
		return NULL;
	}
	trace = get_ce(prop);
	fipath = create_fipath(prop, trace);
	generate_and_append_disjunc(prop, fipath);
	newProp = create_new_expr(prop);
/*
	free_disjunc(L_q_i)
	free_fipath(fipath)
*/
	nextce_debug(5, "%s: Exit", fname); 
	return Prop_create_partial(newProp, Prop_get_type(prop));
}

/**
 * Display the next counter example of the given property.
 * @param prop The property whose counter examples to display
 * @param options The parsed command line options
 * @return 0 if prop has no more counter examples - i.e. prop is now true
 */
int displayNextCE(Prop_ptr prop, const options_t * options) {
	static const char * fname = __func__;
	Prop_ptr new_prop;
	NextCE_ptr nextce;
	/* TODO We may need to filter properties */
	nextce_debug(5, "%s: Enter", fname); 
	new_prop = create_updated_prop(prop, options);
	if (!new_prop) {
		return 0;
	}
	if (is_nextce_debug(5)) {
		printf("%s: About to verify property: ", fname); 
		Prop_print(new_prop, stdout, PROP_PRINT_FMT_FORMULA);
		printf("\n");
	}
	Prop_verify(new_prop); // Also prints
	nextce = Prop_get_nextce_data(prop);
	if (!nextce) {
		nextce = NextCE_create();
		Prop_set_nextce_data(prop, nextce);
	}
	if (Prop_get_status(new_prop) == Prop_True) {
		NextCE_set_status(nextce, NextCE_True);
		fprintf(stdout, "No more counterexamples\n");
		return 0;
	}
	NextCE_set_status(nextce, NextCE_False);
	Prop_set_trace(prop, Prop_get_trace(new_prop));
	nextce_debug(5, "%s: There may be more counterexamples", fname); 
	return 1;
}

/**
 * Select a property according to the command line arguments (1 by name, 1 by
 * number/index, or all properties), and display their next counter example
 * @param options The command line options structure
 * @return 0
 */
int nextceDo(const options_t * options) {
	static const char * fname = __func__;
	int cnt;
	PropDb_ptr propdb = PropPkg_get_prop_database();
	if (options->prop_num != -1) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb, options->prop_num);
		displayNextCE(prop, options);
		return 0;
	}
	/* Iterate all properties */
	for (cnt = 0; cnt < PropDb_get_size(propdb); cnt++) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb, cnt);
		displayNextCE(prop, options);
	}
	return 0;
}

/**
 * Reset the given property with respect to nextce data - i.e. reset the nextce
 * internal structer. In case one does not exist, it will be created.
 * Status is set to 'Reset'.
 * @param prop The property to reset
 * @param options the command line options
 */
static void resetceDoOne(Prop_ptr prop, const options_t * options) {
	NextCE_ptr nextce = Prop_get_nextce_data(prop);
	if (!nextce) {
		nextce = NextCE_create();
		Prop_set_nextce_data(prop, nextce);
	} else {
		NextCE_clear_disjuncts(nextce);
	}
	NextCE_set_status(nextce, NextCE_Reset);
}

/**
 * Select a property according to the command line arguments (1 by name, 1 by
 * number/index, or all properties), and reset it with respect to nextce data.
 * @param options The command line options structure
 * @return 0
 */
int resetceDo(const options_t * options) {
	static const char * fname = __func__;
	int cnt;
	PropDb_ptr propdb = PropPkg_get_prop_database();
	if (options->prop_num != -1) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb, options->prop_num);
		resetceDoOne(prop, options);
		return 0;
	}
	/* Iterate all properties */
	for (cnt = 0; cnt < PropDb_get_size(propdb); cnt++) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb, cnt);
		resetceDoOne(prop, options);
	}
	return 0;
}

/**
 * Print usage information for the given command name to standard error.
 * @param name The command name (nextce, resetce, compute_all).
 * @return 1
 */
int NextCEUsage(const char * name) {
	fprintf(nusmv_stderr, "Usage: %s [-h] [ [ -n index ] | [ -P name ] ]\n", name);
	fprintf(nusmv_stderr, "  -h \t\tPrints this message\n");
	fprintf(nusmv_stderr, "  -n \t\tDisplay next counter example for property numbered 'index'\n");
	fprintf(nusmv_stderr, "  -P \t\tDisplay next counter example for property named 'name'\n");
	return 1;
}

/**
 * Parse the command line arguments (with respect to the invoked command, if it
 * makes a difference), and populate the given command line options structure
 * with the relevant information.
 * @param options The command line options structure to populate
 * @param name The invoked command's name
 * @param argc The number of command line arguments
 * @param argv A C array of C string - the command line arguments themselves
 * @return 0 on success, 1 on failure
 */
int populateOptions(options_t * options, const char * name, int argc, char ** argv) {
	static const char * fname = __func__;
	int c;
	nextce_debug(5, "%s: Enter", fname); 
	util_getopt_reset();
	while ((c = util_getopt(argc, argv, "hn:P:")) != EOF) {
		switch (c) {
		case 'h':
			return NextCEUsage(name);
		case 'n':
			if (options->prop_num != -1) {
				return NextCEUsage(name);
			}
			options->prop_num = PropDb_get_prop_index_from_string(
					PropPkg_get_prop_database(),
					util_optarg);
			if (options->prop_num == -1) {
				return(1);
			}
			break;
		case 'P': {
			char * formula_name;
			if (options->prop_num != -1) {
				return NextCEUsage(name);
			}
			formula_name = util_strsav(util_optarg);
			options->prop_num = PropDb_get_prop_index_from_string(
					PropPkg_get_prop_database(),
					util_optarg);
			if (options->prop_num == -1) {
				fprintf(nusmv_stderr, "No property named '%s'\n",
						formula_name);
				FREE(formula_name);
				return(1);
			}
			FREE(formula_name);
			break;
		}
		}
	}
	if (argc != util_optind) {
		return NextCEUsage(name);
	}
	debug_show_options(fname, options);
	return 0;
}

/**
 * This method is called when the user invoked 'nextce ...'
 * @param argc The number of command line arguments
 * @param argv A C array of C string - the command line arguments themselves
 * @return 0 on success, non-0 on failure
 */
int CommandCENextCE(int argc, char ** argv) {
	static const char * fname = "CommandCENextCE";
	int rc;
	options_t options = {-1};
	rc = populateOptions(&options, "next_ce", argc, argv);
	if (rc != 0) {
		return rc;
	}
	rc = nextceDo(&options);
	nextce_debug(5, "%s: Exit", fname); 
	return rc;
}

/**
 * Reset all properties with respect to nextce information.
 */
void resetAllCE() {
	options_t options = {-1};
	resetceDo(&options);
}

/**
 * This method is called when the user invoked 'resetce ...'
 * @param argc The number of command line arguments
 * @param argv A C array of C string - the command line arguments themselves
 * @return 0 on success, non-0 on failure
 */
int CommandCEResetCE(int argc, char ** argv) {
	static const char * fname = "CommandCEResetCE";
	int rc;
	options_t options = {-1};
	nextce_debug(5, "%s: Enter", fname); 
	rc = populateOptions(&options, "reset_ce", argc, argv);
	if (rc != 0) {
		return rc;
	}
	rc = resetceDo(&options);
	nextce_debug(5, "%s: Exit", fname); 
	return rc;
}

/**
 * Compute all counter examples and display them for the given single property.
 * Continues from where invocations to next_ce left off.
 * @param prop The property to display
 * @param options Parsed command line options
 */
void compuateAllDoOne(Prop_ptr prop, options_t * options) {
	int rc;
	do {
		rc = displayNextCE(prop, options);
	} while (rc != 0);
}

/**
 * Select a property according to the command line arguments (1 by name, 1 by
 * number/index, or all properties), and display all its counter examples
 * @param options The command line options structure
 * @return 0
 */
int computeAllDo(options_t * options) {
	static const char * fname = __func__;
	int cnt;
	PropDb_ptr propdb = PropPkg_get_prop_database();
	if (options->prop_num != -1) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb,
				options->prop_num);
		compuateAllDoOne(prop, options);
		return 0;
	}
	/* Iterate all properties */
	for (cnt = 0; cnt < PropDb_get_size(propdb); cnt++) {
		Prop_ptr prop = PropDb_get_prop_at_index(propdb, cnt);
		compuateAllDoOne(prop, options);
	}
	return 0;
}

/**
 * This method is called when the user invoked 'compute_all ...'
 * @param argc The number of command line arguments
 * @param argv A C array of C string - the command line arguments themselves
 * @return 0 on success, non-0 on failure
 */
int CommandCEComputeAll(int argc, char ** argv) {
	static const char * fname = "CommandCEComputeAll";
	options_t options = {-1};
	int rc;
	nextce_debug(5, "%s: Enter", fname); 
	rc = populateOptions(&options, "compute_all", argc, argv);
	if (rc != 0) {
		return rc;
	}
	rc = computeAllDo(&options);
	nextce_debug(5, "%s: Exit", fname); 
	return rc;
}

/**
 * This integer holds the set equivalence_class, as set by calls to set
 * ce_equivalence.
 * Note: I despise globals, but creating a global instance for a single integer
 * seems a bit much. If we ever have more than this single int as a global, we
 * should switch over. */
static int equivalence_class = 1;

/**
 * Getter function for the global equivalnce_class registry value.
 * This method is used to retrieve the equivalence class set with the call:
 * 'set ce_equivalence'
 * @return The set equivalnce_class.
 */
int NextCE_get_equivalence_class() {
	return equivalence_class;
}

/**
 * Setter function for the global equivalnce_class registry value.
 * This method is invoked when the user calls 'set ce_equivalence'
 * @param class The new equivalnce_class to be set.
 */
void NextCE_set_equivalence_class(int class) {
	static const char * fname = __func__;
	nextce_debug(5, "%s: Enter with %d", fname, class);
	resetAllCE();
	equivalence_class = class;
}

