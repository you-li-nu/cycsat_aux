#include "solver.h"
#include "util.h"
#include "sim.h"
#include "sld.h"

#include <iterator>
#include <algorithm>
#include <boost/lexical_cast.hpp>

#include <ilcplex/ilocplex.h>
ILOSTLBEGIN

solver_t::solver_t(ckt_n::ckt_t& c, ckt_n::ckt_t& s, int verb)
    : ckt(c)
    , simckt(s)
    , sim(s, s.ckt_inputs)
    , dbl(c, ckt_n::dup_allkeys, true)
    , input_values(ckt.num_ckt_inputs(), false)
    , output_values(ckt.num_outputs(), false)
    , fixed_keys(ckt.num_key_inputs(), false)
    , verbose(verb)
    , iter(0)
    , backbones_count(0)
    , cube_count(0)
{
    MAX_VERIF_ITER = 1;
    time_limit = 1e100;

    using namespace ckt_n;
    using namespace sat_n;
    using namespace AllSAT;

    assert(dbl.dbl->num_outputs() == 1);
    assert(dbl.ckt.num_ckt_inputs() == ckt.num_ckt_inputs());

    // dbl.dbl->split_gates();
    // dbl.dbl->dump(std::cout);

    dbl.dbl->init_solver(S, cl, lmap, true);
    node_t* out = dbl.dbl->outputs[0];
    l_out = lmap[out->get_index()];
    cl.verbose = verbose;

    if(verbose) {
        std::cout << dbl.ckt << std::endl;
        std::cout << *dbl.dbl << std::endl;
        std::cout << "DBL MAPPINGS" << std::endl;
        dump_mappings(std::cout, *dbl.dbl, lmap);
    }

    // setup arrays of literals.
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        int idx = ckt.key_inputs[i]->get_index();
        keyinput_literals_A.push_back(lmap[dbl.pair_map[idx].first->get_index()]);
        keyinput_literals_B.push_back(lmap[dbl.pair_map[idx].second->get_index()]);
    }
    for(unsigned i=0; i != ckt.num_ckt_inputs(); i++) {
        int idx = ckt.ckt_inputs[i]->get_index();
        cktinput_literals.push_back(lmap[dbl.pair_map[idx].first->get_index()]);
    }
    for(unsigned i=0; i != ckt.num_outputs(); i++) {
        int idx = ckt.outputs[i]->get_index();
        node_t* outA = dbl.pair_map[idx].first;
        node_t* outB = dbl.pair_map[idx].second;
        Lit lA = lmap[outA->get_index()];
        Lit lB = lmap[outB->get_index()];
        output_literals_A.push_back(lA);
        output_literals_B.push_back(lB);
    }

    S.freeze(keyinput_literals_A);
    S.freeze(keyinput_literals_B);
    S.freeze(cktinput_literals);
    S.freeze(output_literals_A);
    S.freeze(output_literals_B);
    S.freeze(l_out);

    dbl_keyinput_flags.resize(S.nVars(), false);
    dbl.dbl->init_keyinput_map(lmap, dbl_keyinput_flags);
}


void solver_t::addKnownKeys(std::vector<std::pair<int, int> >& values)
{
    for(unsigned i=0; i != values.size(); i++) {
        using namespace sat_n;
        Lit lA = keyinput_literals_A[values[i].first];
        Lit lB = keyinput_literals_B[values[i].first];
        assert(values[i].second == 0 || values[i].second == 1);
        if(values[i].second) {
            S.addClause(lA);
            S.addClause(lB);
        } else {
            S.addClause(~lA);
            S.addClause(~lB);
        }
    }
}

solver_t::~solver_t()
{
}

bool solver_t::solve(solver_t::solver_version_t ver, rmap_t& keysFound, bool quiet)
{
    assert( SOLVER_V0 == ver );
    return _solve_v0(keysFound, quiet, -1);
}

void solver_t::_record_sim(
    const std::vector<bool>& input_values,
    const std::vector<bool>& output_values,
    std::vector<sat_n::lbool>& values
)
{
    using namespace sat_n;
    using namespace ckt_n;
    using namespace AllSAT;

    iovectors.push_back(iovalue_t());
    int last = iovectors.size()-1;
    iovectors[last].inputs = input_values;
    iovectors[last].outputs = output_values;

    // extract inputs and put them in the array.
    for(unsigned i=0; i != input_values.size(); i++) {
        lbool val = input_values[i] ? l_True : l_False;
        int jdx  = dbl.dbl->ckt_inputs[i]->get_index();
        assert(var(lmap[jdx]) < values.size());
        assert(var(lmap[jdx]) >= 0);
        values[var(lmap[jdx])] = val;

    }

    // and then the outputs.
    for(unsigned i=0; i != ckt.num_outputs(); i++) {
        node_t* n_i = ckt.outputs[i];
        int idx = n_i->get_index();
        int idxA = dbl.pair_map[idx].first->get_index();
        int idxB = dbl.pair_map[idx].second->get_index();
        Var vA = var(lmap[idxA]);
        Var vB = var(lmap[idxB]);
        assert(vA < values.size() && vA >= 0);
        assert(vB < values.size() && vB >= 0);
        if(output_values[i] == true) {
            values[vA] = values[vB] = sat_n::l_True;
        } else {
            values[vA] = values[vB] = sat_n::l_False;
        }
    }
}

// Evaluates the output for the values stored in input_values and then records
// this in the solver.
void solver_t::_record_input_values()
{
    std::vector<sat_n::lbool> values(S.nVars(), sat_n::l_Undef);
    sim.eval(input_values, output_values);
    _record_sim(input_values, output_values, values);
    int cnt = cl.addRewrittenClauses(values, dbl_keyinput_flags, S);
    __sync_fetch_and_add(&cube_count, cnt);
}

void solver_t::_cycle_search(ckt_n::node_t* r, ckt_n::nodelist_t* cycle_list) {
	using namespace ckt_n;

	if (r->visit_status == 2) {
		return;
	}
	r->visit_status = 1;
	for (unsigned i = 0; i != r->num_inputs(); i++) {
		node_t* g = r->inputs[i];
//	std::cout << r->name << ",\t" << r->func << std::endl;
//	std::cout << g->name << ",\t" << g->func << std::endl << std::endl;
		if (g->visit_status == 1) {
			if (g->feedback == 0) {
				g->feedback = 1;
				cycle_list->push_back(g);
			}
		}else {
			_cycle_search(g, cycle_list);
		}
	}
//	std::cout << r->name << ",\t" << r->get_index() << std::endl;
	r->visit_status = 2;

}

bool solver_t::_topo_sort(ckt_n::nodelist_t* order, ckt_n::node_t* start, ckt_n::node_t* end) {
	using namespace ckt_n;

	if (start->visit_status == 1) {
		if (start == end)
			return true;
	  	else return false;
	}else if (start->visit_status == 2) {
		return false;
	}else if (start->visit_status == 3) {
		return true;
	}

//	std::cout << std::endl << start->name << ":" << std::endl;
	start->visit_status = 1;
	bool tmp = false;
	for (unsigned i = 0; i != start->num_inputs(); i++) {
		node_t* g = start->inputs[i];
//		std::cout << g->name << " " << g->visit_status << ",\t";
		tmp = _topo_sort(order, g, end) || tmp;
	}
//	std::cout <<std::endl << tmp << std::endl;
	if (tmp) {
		order->push_back(start);
		start->visit_status = 3;
	}else {
		start->visit_status = 2;
	}
	return tmp;
}

void solver_t::_add_lit(std::vector<sat_n::vec_lit_t*>* clauses, std::vector<sat_n::vec_lit_t*>* in, int index, bool value) {
	using namespace sat_n;
	vec_lit_t clause;
	clause.push(Lit(var(lmap[index]), value));

	_add_lits(clauses, in, &clause);
}

void solver_t::_add_lits(std::vector<sat_n::vec_lit_t*>* clauses, std::vector<sat_n::vec_lit_t*>* in, sat_n::vec_lit_t* c) {
	using namespace sat_n;

	for (unsigned i = 0; i != in->size(); i++) {
		vec_lit_t* clause = (*in)[i];
		vec_lit_t* tmp = new vec_lit_t();

		for (unsigned j = 0; j != clause->size(); j++) {
			tmp->push((*clause)[j]);
		}
		for (unsigned j = 0; j != c->size(); j++) {
			tmp->push((*c)[j]);
		}
		clauses->push_back(tmp);
	}
}

void solver_t::_append_lit_vec(std::vector<sat_n::vec_lit_t*>* dest, std::vector<sat_n::vec_lit_t*>* origin) {
	using namespace sat_n;

	for (unsigned i = 0; i != origin->size(); i++) {
		vec_lit_t* ori_clause = (*origin)[i];
		vec_lit_t* tmp = new vec_lit_t();
		for (unsigned j = 0; j != ori_clause->size(); j++) {
			tmp->push((*ori_clause)[j]);
		}
		dest->push_back(tmp);
	}
}

//zero means cannot break.
//We assert for every visited node, either it->zero or it->var is true.
void solver_t::_add_key_constraint(ckt_n::nodelist_t* order) {
    using namespace std;
    using namespace ckt_n;
    using namespace sat_n;

    //std::cout << "No. Clauses: " << S.nClauses() << std::endl;
    //std::cout << "No. Variables: " << S.nVars() << std::endl;

    for (unsigned i = 0; i != order->size(); i++) {
        node_t* g = (*order)[i];
        string func = g->func;
		//std::cout << g->name << " " << g->func << std::endl;

        if (func == "mux") {
            node_t* select = g->inputs[0];
            node_t* in0 = g->inputs[1];
            node_t* in1 = g->inputs[2];
			//std::cout << "in0: " << in0->name << std::endl;
			//std::cout << "in1: " << in1->name << std::endl;
            int index = select->get_index();

            if (in0->zero && in1->zero) {
				//std::cout << "130" << std::endl;
                g->zero = true;
				//std::cout << "130" << std::endl;
            }
            else if (select->keyinput) {
                g->var = true;
                Lit y = g->lit = CMSat::Lit(S.newVarDump(g->get_index()), false);
                Lit s = CMSat::Lit(var(lmap[index]), false);
				if (in0->zero) {
					if (in1->var) {
						S.addClause(y, ~s, ~in1->lit);
						S.addClause(~y, s);
						S.addClause(~y, in1->lit);
					}
					else {//!in1->var&&!in1->zero, which means it is not visited yet.
						//std::cout << in0->zero << in0->var << in1->zero << in1->var <<std::endl;
						S.addClause(y, ~s);
						S.addClause(~y, s);
					}
				}
				/*else {//else if? What if none is zero?
					if (in0->var) {
							S.addClause(y, s, ~in0->lit);
							S.addClause(~y, ~s);
							S.addClause(~y, in0->lit);
						}
						else {
							S.addClause(y, s);
							S.addClause(~y, ~s);
						}
				}*/
				else if (in1->zero) {
					if (in0->var) {
						S.addClause(y, s, ~in0->lit);
						S.addClause(~y, ~s);
						S.addClause(~y, in0->lit);
					}
					else {//!in0->var&&!in0->zero, which means it is not visited yet.
						S.addClause(y, s);
						S.addClause(~y, ~s);
					}
				}
			    else {//none of in0 or in1 is zero
					assert(in0->var||in1->var);//It cannot be the case that all zero and var fields of both in0 and in1 be false.
					if (in0->var && in1->var) {
		                S.addClause(s, ~in0->lit, y);
		                S.addClause(s, in0->lit, ~y);
		                S.addClause(~s, ~in1->lit, y);
		                S.addClause(~s, in1->lit, ~y);
					}
					else if (in0->var) {//in1 is not visited
		                S.addClause(s, ~in0->lit, y);
		                S.addClause(s, in0->lit, ~y);
		                S.addClause(~s, y);
					}
					else {//in1->var, in0 is not visited
		                S.addClause(s, y);
		                S.addClause(~s, ~in1->lit, y);
		                S.addClause(~s, in1->lit, ~y);
					}
			    }
			    
			
            }
            
            /*else {
                if (in0->zero || in1->zero) {
                    g->zero = true;
                }
                else {
                    g->var = true;
                    Lit y = g->lit = CMSat::Lit(S.newVarDump(g->get_index() + 1), false);
                    if (in0->var && in1->var) {
                        Lit a = in0->lit;
                        Lit b = in1->lit;
                        S.addClause(~y, a);
                        S.addClause(~y, b);
                        S.addClause(y, ~a, ~b);
                    }
                    else {
                        Lit i;
                        if (in0->var) {
                            i = in0->lit;
                        }
                        else if (in1->var) {
                            i = in1->lit;
                        }
						else {
							std::cerr << "Both inputs of a MUX are not visited." << std::endl;
						}
                        S.addClause(~y, i);
                        S.addClause(y, ~i);
                    }
                }
            }*/
            else{
	            if (in0->zero || in1->zero || select->zero) {
				  g->zero = true;
				}else {
				  g->var = true;
				  Lit y = g->lit = CMSat::Lit(S.newVarDump(g->get_index() + 1), false);
				  if (in0->var && in1->var && select->var) {
				    Lit a = in0->lit;
				    Lit b = in1->lit;
				    Lit c = select->lit;
				    S.addClause(~y, a);
				    S.addClause(~y, b);
				    S.addClause(~y, c);
				    S.addClause(y, ~a, ~b, ~c);
				  }else if (in0->var && in1->var) {
				    Lit a = in0->lit;
				    Lit b = in1->lit;
				    S.addClause(~y, a);
				    S.addClause(~y, b);
				    S.addClause(y, ~a, ~b);
				  }else if (in0->var && select->var) {
				    Lit a = in0->lit;
				    Lit b = select->lit;
				    S.addClause(~y, a);
				    S.addClause(~y, b);
				    S.addClause(y, ~a, ~b);
				  }else if (in1->var && select->var) {
				    Lit a = in1->lit;
				    Lit b = select->lit;
				    S.addClause(~y, a);
				    S.addClause(~y, b);
				    S.addClause(y, ~a, ~b);
				  }else {
				    Lit i;
				    if (in0->var) {
				      i = in0->lit;
				    }else if (in1->var) {
				      i = in1->lit;
				    }else {
				      i = select->lit;
				    }
				    S.addClause(~y, i);
				    S.addClause(y, ~i);
				  }
				}
			}
      
        }

        else {
            bool zero = false, key = false;
            Lit k;
			//std::cout << "No inputs: " << g->num_inputs() << std::endl;
            for (unsigned j = 0; j != g->num_inputs(); j++) {
                node_t* in = g->inputs[j];
                if (in->keyinput && (func == "and" || func == "nand")){
                    assert(!key);//Assuming only one key each gate
                    key = true;
                    k = Lit(var(lmap[in->get_index()]), true);
                }
                else if (in->keyinput && (func == "or" || func == "nor")){
                    assert(!key);
                    key = true;
                    k = Lit(var(lmap[in->get_index()]), false);
                }
                else if (in->zero)
                    zero = true;
	        }
            if (!key && zero)//Do not has key and has zeros
                g->zero = true;
            else {
                vec_lit_t clause;
                g->var = true;
                Lit y = g->lit = CMSat::Lit(S.newVarDump(g->get_index()), false);

                clause.push(y);
                for (unsigned j = 0; j != g->num_inputs(); j++) {
					//std::cout << "Processing input No. :" << j << std::endl;
                    node_t* in = g->inputs[j];
					//std::cout << "name: " << in->name << " index: " << in->index << std::endl;
				    if (in->keyinput) {
						std::cout << "A keyinput." << std::endl;
						continue;
				    }
                    if (in->var || in->zero) {
                        if (in->var) {
                            Lit i = in->lit;
                            if (key){
				                S.addClause(~y, i, k);
							}
                            else{
								/*std::cout << in->var << std::endl;
								std::cout << in->zero << std::endl;
								std::cout << sign(y) << " " << + toInt(y) << std::endl;
								std::cout << sign(i) << " " << + toInt(i) << std::endl;*/
				                S.addClause(~y, i);
							}
                        }
                        else {//in->zero
                            S.addClause(~y, k);
                        }

						Lit v = CMSat::Lit(S.newVarDump(g->get_index()), false);
                        clause.push(v);
						if (key && in->var) {
                            S.addClause(~v, ~k);
                            S.addClause(~v, ~(in->lit));
                        }
                        else if (key){//key && in->zero
                            S.addClause(~v, ~k);
						}
                        else if (in->var){//no key
                            S.addClause(~v, ~(in->lit));
						}
						else{}
				
	                        /*if (key && in->var) {
	                            Lit v = mkLit(S.newVarDump(g->get_index()));
	                            clause.push(v);
	                            S.addClause(~v, ~k);
	                            S.addClause(~v, ~(in->lit));
	                        }
	                        else if (key)
	                            clause.push(~k);
	                        else
	                            clause.push(~in->lit);
							*/
					}
				}
                S.addClause(clause);
            }
        }
    }
    unsigned i = order->size()-1;
    node_t* g = (*order)[i];
    assert(g->var && g->feedback == 1);
    S.addClause(g->lit);
    for (unsigned i = 0; i != order->size(); i++) {
        (*order)[i]->var = false;
        (*order)[i]->zero = false;
    }

    //std::string filename = "stdout";
    //S.writeCNF(filename);
    //std::cout << "No. Clauses: " << S.nClauses() << std::endl;

}

bool solver_t::_solve_v0(rmap_t& keysFound, bool quiet, int dlimFactor)
{
    using namespace sat_n;
    using namespace ckt_n;
    using namespace AllSAT;
    using namespace std;


    // rjn6330 start
	nodelist_t cycle_list, topo_order;

    node_t* g = dbl.dbl->outputs[0];
	_cycle_search(g, &cycle_list);

	//std::cout << "Cycle list size: " << cycle_list.size() << std::endl;//youl

    for(unsigned i=0; i != cycle_list.size(); i++) {
    	for(unsigned j=0; j != dbl.dbl->num_gates(); j++) {
    		node_t* g = dbl.dbl->gates[j];
			g->visit_status = 0;
			g->zero = false;
			g->var = false;
		}
    	node_t* start = cycle_list[i];

		_topo_sort(&topo_order, start, start);
		//std::cout << start->name << std::endl;
		start->zero = true;

		//std::cout << "cycle:\t" << start->name << std::endl;
	   	//for(unsigned k=0; k != topo_order.size(); k++) {
    		//node_t* g = topo_order[k];
			//std::cout << " <= " << g->name;
		//}
		//std::cout << std::endl;

		_add_key_constraint(&topo_order);

		topo_order.clear();
	}
    //std::cout << "No. Clauses: " << S.nClauses() << std::endl;

//	std::cout << std::endl;

    /*

    for(unsigned i=0; i != dbl.dbl->num_inputs(); i++) {
    	node_t* g = dbl.dbl->inputs[i];
		std::cout << "inputs\t" << g->name << '\t' << var(lmap[g->get_index()]) << std::endl;
	}
    for(unsigned i=0; i != dbl.dbl->num_gates(); i++) {
    	node_t* g = dbl.dbl->gates[i];
		std::cout << "gates\t" << g->name << '\t' << g->func << '\t' << var(lmap[g->get_index()]) << std::endl;
	}
	*/
	//youl


    // rjn6330 end

    // add all zeros.
    for(unsigned i=0; i != dbl.dbl->num_ckt_inputs(); i++) {
        input_values[i]=false;
    }
    _record_input_values();

    // and all ones.
    for(unsigned i=0; i != dbl.dbl->num_ckt_inputs(); i++) {
        input_values[i]=true;
    }
    _record_input_values();

    bool done = false;
	int i = 0;
    while(true) {
		i++;
		if (i == 2) {
			//break;
		}
        bool result = S.solve(l_out);
        if(dlimFactor != -1) {
            int dlim = dlimFactor * S.nVars();
            if(dlim <= S.getNumDecisions()) {
                std::cout << "too many decisions! giving up." << std::endl;
                break;
            }
        }

        __sync_fetch_and_add(&iter, 1);
        //std::cout << "iteration #" << iter << std::endl;
        //std::cout << "vars: " << S.nVars() << "; clauses: " << S.nClauses() << std::endl;
        // std::string filename = "solver" + boost::lexical_cast<std::string>(iter) + ".cnf";
        //S.writeCNF(filename);

        if(verbose) {
            dbl.dump_solver_state(std::cout, S, lmap);
            std::cout << std::endl;
        }
        std::cout << "iteration: " << iter
                  << "; vars: " << S.nVars()
                  << "; clauses: " << S.nClauses()
                  << "; decisions: " << S.getNumDecisions() << std::endl;

        if(false == result) {
            done = true;
            break;
        }

        // now extract the inputs.
		string str = "";
        for(unsigned i=0; i != dbl.dbl->num_ckt_inputs(); i++) {
            int jdx  = dbl.dbl->ckt_inputs[i]->get_index();
            lbool val = S.modelValue(lmap[jdx]);
            assert(val.isDef());
            if(!val.getBool()) {
				str += "0";
                input_values[i] = false;
            } else {
				str += "1";
                input_values[i] = true;
            }
        }
		std::cout << str << std::endl;
        _record_input_values();
        if(verbose) {
            std::cout << "input: " << input_values
                << "; output: " << output_values << std::endl;
        }

        // _sanity_check_model();

        struct rusage ru_current;
        getrusage(RUSAGE_SELF, &ru_current);
        if(utimediff(&ru_current, &ru_start) > time_limit) {
            std::cout << "timeout in the slice loop." << std::endl;
            break;
        }
    }
    if(done) {
        std::cout << "finished solver loop." << std::endl;
        _verify_solution_sim(keysFound);
    }
    return done;
#if 0
    _verify_solution_sat();
#endif
}

void solver_t::_sanity_check_model()
{
    using namespace sat_n;
    using namespace ckt_n;

    bool pass = true;
    vec_lit_t assumps;
    std::vector<bool> actual_output_values;

    for(unsigned i=0; i != cktinput_literals.size(); i++) {
        bool vi = input_values[i];
        assumps.push( vi ? cktinput_literals[i] : ~cktinput_literals[i]);
    }
    if(verbose) dump_clause(std::cout << "assumps: ", assumps) << std::endl;
    if(S.solve(assumps) == false) {
        std::cout << "UNSAT result during sanity check." << std::endl;
        std::cout << "result of no-assumption solve: " << S.solve() << std::endl;
        exit(1);
    }
    if(verbose) {
        std::cout << "[expected] input: " << input_values
            << "; output: " << output_values << std::endl;
    }

    if(verbose) {
        dump_clause(std::cout << "sat input: ", assumps) << std::endl;
        std::cout << "sat output: ";
    }
    for(unsigned i=0; i != output_values.size(); i++) {
        bool vi = output_values[i];
        lbool ri = S.modelValue(output_literals_A[i]);
        if(verbose) {
            std::cout << (ri.isUndef() ? "-" : (ri.getBool() ? "1" : "0"));
        }
        if(!(ri.isDef() && ri.getBool() == vi)) {
            pass = false;
        }
    }
    if(verbose) std::cout << std::endl;

    if(pass) {
        if(verbose) {
            std::cout << "simulation sanity check passed." << std::endl;
        }
    } else {
        std::cout << "simulation sanity check failed." << std::endl;
        exit(1);
    }
}

void solver_t::blockKey(rmap_t& keysFound)
{
    using namespace sat_n;

    vec_lit_t bc_A(keyinput_literals_A.size());
    vec_lit_t bc_B(keyinput_literals_A.size());
    for(unsigned i=0; i != keyinput_literals_A.size(); i++) {
        auto &&name_i = ckt.key_inputs[i]->name;
        assert (keysFound.find(name_i)  != keysFound.end());
        auto ki = keysFound[ckt.key_inputs[i]->name];
        bc_A[i] = ki ? ~keyinput_literals_A[i] : keyinput_literals_A[i];
        bc_B[i] = ki ? ~keyinput_literals_B[i] : keyinput_literals_B[i];
    }
    S.addClause(bc_A);
    S.addClause(bc_B);
}

bool solver_t::getNewKey(rmap_t& keysFound)
{
    keysFound.clear();
    return _verify_solution_sim(keysFound);
}

bool solver_t::_verify_solution_sim(rmap_t& keysFound)
{
    using namespace sat_n;
    using namespace ckt_n;

    srand(142857142);
    bool pass = true;
    for(int iter=0; iter < MAX_VERIF_ITER;  iter++) {
        vec_lit_t assumps;
        std::vector<bool> input_values;
        std::vector<bool> output_values;

        for(unsigned i=0; i != cktinput_literals.size(); i++) {
            bool vi = bool(rand() % 2);
            assumps.push( vi ? cktinput_literals[i] : ~cktinput_literals[i]);
            input_values.push_back(vi);
        }
        sim.eval(input_values, output_values);
        if(verbose) {
            std::cout << "input: " << input_values
                      << "; output: " << output_values << std::endl;
            dump_clause(std::cout << "assumps: ", assumps) << std::endl;
        }

        if(S.solve(assumps) == false) {
            std::cout << "UNSAT model!" << std::endl;
            return false;
        }

        if(verbose) {
            dump_clause(std::cout << "sat input: ", assumps) << std::endl;
            std::cout << "sat output: ";
        }
        for(unsigned i=0; i != output_values.size(); i++) {
            bool vi = output_values[i];
            lbool ri = S.modelValue(output_literals_A[i]);
            if(verbose) {
                std::cout << (ri.isUndef() ? "-" : (ri.getBool() ? "1" : "0"));
            }
            if(!(ri.isDef() && ri.getBool() == vi)) {
                pass = false;
            }
        }
        if(iter == 0) {
            _extractSolution(keysFound);
        }
        if(verbose) std::cout << std::endl;
        if(!pass) {
            if(verbose) {
                dbl.dump_solver_state(std::cout, S, lmap);
                std::cout << std::endl;
            }
            std::cout << "sim failed." << std::endl;
            break;
        }
    }
    return pass;
}

void solver_t::_extractSolution(rmap_t& keysFound)
{
    using namespace sat_n;

    for(unsigned i=0; i != keyinput_literals_A.size(); i++) {
        lbool v = S.modelValue(keyinput_literals_A[i]);
        if(!v.getBool()) {
            keysFound[ckt.key_inputs[i]->name] = 0;
        } else {
            keysFound[ckt.key_inputs[i]->name] = 1;
        }
    }
}

bool solver_t::_verify_solution_sat()
{
    using namespace sat_n;
    vec_lit_t c1, c2;

    assert(keyinput_literals_A.size() == keyinput_literals_B.size());
    c1.growTo(keyinput_literals_A.size()+1);
    c2.growTo(keyinput_literals_B.size()+1);

    for(unsigned i=0; i != keyinput_literals_A.size(); i++) {
        c1[i+1] = ~keyinput_literals_A[i];
        c2[i+1] = ~keyinput_literals_B[i];
    }
    c1[0] = l_out;
    c2[0] = l_out;
    if(S.solve(c1) == false && S.solve(c2) == false) {
        return true;
    } else {
        return true;
    }
}

int solver_t::sliceAndSolve( rmap_t& keysFoundMap, int maxKeys, int maxNodes )
{
    using namespace ckt_n;
    using namespace sat_n;

    if(maxNodes != -1) {
        assert(maxNodes <= (int) ckt.nodes.size());
        assert(maxNodes > 0);
    }
    if(maxKeys != -1) {
        assert(maxKeys <= (int) ckt.num_key_inputs());
        assert(maxKeys > 0);
    }

    // initialize the maps.
    assert(ckt.gates.size() == ckt.gates_sorted.size());
    assert(ckt.nodes.size() == ckt.nodes_sorted.size());

    std::vector< std::vector<bool> > keymap, outputmap;
    ckt.init_keymap(keymap);
    ckt.init_outputmap(outputmap);

    IloEnv env;

    int numNodeVars = ckt.num_nodes();
    int numOutputVars = ckt.num_outputs();
    int numKeyVars = ckt.num_key_inputs();
    int totalVars = numNodeVars + numOutputVars + numKeyVars;

    auto outVarIndex = [numNodeVars](int i) -> int
        { return numNodeVars + i; };
    auto keyVarIndex = [numNodeVars, numOutputVars](int i) -> int
        { return numNodeVars + numOutputVars + i; };


    // first create the variables.
    IloNumVarArray vars(env);
    int i;
    for(i=0; i != numNodeVars; i++) {
        char name[64];
        sprintf(name, "n_%s_%d", ckt.nodes[i]->name.c_str(), i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
    }
    for(; i < numNodeVars+numOutputVars; i++) {
        char name[64];
        sprintf(name, "o_%s_%d", ckt.outputs[i-numNodeVars]->name.c_str(), i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
        assert(outVarIndex(i-numNodeVars) == i);
    }
    for(; i < numNodeVars+numOutputVars+numKeyVars; i++) {
        char name[64];
        sprintf(name, "k_%s_%d", ckt.key_inputs[i-numNodeVars-numOutputVars]->name.c_str(), i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
        assert(keyVarIndex(i-numNodeVars-numOutputVars) == i);
    }
    assert(i == totalVars);

    // and then the constraints.
    IloRangeArray cnst(env);
    int ccPos=0; // current constraint position.

    // for each key ki: ki => oj for each ouput j that is affected bby ki
    // ki => oj translates to -ki + oj >= 0
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        std::vector<int> output_indices;
        ckt.get_indices_in_bitmap(outputmap, ckt.key_inputs[i], output_indices);
        for(auto it = output_indices.begin(); it != output_indices.end(); it++) {
            unsigned j=*it;
            int vi = keyVarIndex(i);
            int vj = outVarIndex(j);
            char name[16]; sprintf(name, "c%d", ccPos);
            cnst.add(IloRange(env, 0, IloInfinity, name));
            cnst[ccPos].setLinearCoef(vars[vi], -1);
            cnst[ccPos].setLinearCoef(vars[vj], 1);
            ccPos += 1;
        }
    }

    // for each output oi, and each node nj in the fanin cone of that output:
    // oi => nj <=> -oi + nj >= 0
    ckt.check_sanity();
    for(unsigned i=0; i != ckt.num_outputs(); i++) {
        std::vector<bool> fanin(ckt.num_nodes());
        ckt.compute_transitive_fanin(ckt.outputs[i], fanin);

        for(int j=0; j != (int)fanin.size(); j++) {
            if(fanin[j]) {
                node_t* nj = ckt.nodes[j];
                assert(nj->get_index() == j);
                int vi = outVarIndex(i);
                int vj = j;
                char name[16]; sprintf(name, "c%d", ccPos);
                cnst.add(IloRange(env, 0, IloInfinity, name));
                cnst[ccPos].setLinearCoef(vars[vi], -1);
                cnst[ccPos].setLinearCoef(vars[vj], 1);
                ccPos += 1;
            }
        }
    }

    // now create the objective function
    IloObjective obj;
    IloNumArray coeffs(env, totalVars);
    if(maxKeys == -1) {
        // maximize the number of selected keys.
        obj = IloMaximize(env);
        for(int i=0; i != numKeyVars; i++) {
            coeffs[keyVarIndex(i)] = 1;
        }
    } else {
        // minimize: number of selected nodes.
        obj = IloMinimize(env);
        for(int i=0; i != numNodeVars; i++) {
            coeffs[i] = 1;
        }
    }
    obj.setLinearCoefs(vars, coeffs);

    if(maxNodes != -1) {
        // n1 + n2 + ... + nk <= maxNodes
        char name[16]; sprintf(name, "c%d", ccPos);
        cnst.add(IloRange(env, 0, maxNodes, name));
        int nodeCnstPos = ccPos++;
        for(int i=0; i != numNodeVars; i++) {
            cnst[nodeCnstPos].setLinearCoef(vars[i], 1);
        }
    }
    if(maxKeys != -1) {
        // k1 + k2 + ... + kl >= maxKeys
        char name[16]; sprintf(name, "c%d", ccPos);
        cnst.add(IloRange(env, maxKeys, IloInfinity, name));
        int keyCnstPos = ccPos++;
        for(int i=0; i != numKeyVars; i++) {
            int ki = keyVarIndex(i);
            cnst[keyCnstPos].setLinearCoef(vars[ki], 1);
        }
    } else {
        // k1 + k2 + ... + kl >= 1
        char name[16]; sprintf(name, "c%d", ccPos);
        cnst.add(IloRange(env, 1, IloInfinity, name));
        int keyCnstPos = ccPos++;
        for(int i=0; i != numKeyVars; i++) {
            int ki = keyVarIndex(i);
            cnst[keyCnstPos].setLinearCoef(vars[ki], 1);
        }
    }

    IloModel model(env);
    model.add(obj);
    model.add(cnst);
    IloCplex cplex(model);
    cplex.setOut(env.getNullStream());
    cplex.setParam(IloCplex::TiLim, 1);
    cplex.setParam(IloCplex::EpGap, 0.25);

    static int cnt=0;
    char buf[24]; sprintf(buf, "model%d.lp", cnt++);
    cplex.exportModel(buf);

    if(!cplex.solve()) {
        return 0;
    }

    IloNumArray vals(env);
    cplex.getValues(vals, vars);

    slice_t slice(ckt, simckt);
    for(int i=0; i != numOutputVars; i++) {
        int index = outVarIndex(i);
        int vi = (int) vals[index];
        if(vi) {
            slice.outputs.push_back(i);
        }
    }

    //std::cout << "selected keys: ";
    for(int i =0; i != numKeyVars; i++) {
        int index = keyVarIndex(i);
        int vi = (int) vals[index];
        if(vi) {
            slice.keys.push_back(i);
            //std::cout << i << "/" << index << " ";
        }
    }

    assert(maxNodes < (int) ckt.num_nodes() || slice.keys.size() == ckt.num_key_inputs());
    //std::cout << std::endl;

    std::cout << "# of outputs: " << slice.outputs.size() << std::endl;
    std::cout << "# of keys: " << slice.keys.size() << std::endl;
    std::cout << "objective : " << cplex.getObjValue() << std::endl;

    std::map<int, int> keysFound;
    rmap_t allKeys;
    this->solveSlice(slice, keysFound, allKeys);
    for(auto it = keysFound.begin(); it != keysFound.end(); it++) {
        int keyIndex = it->first;
        int keyValue = it->second;
        const std::string& keyName = ckt.key_inputs[keyIndex]->name;
        keysFoundMap[keyName] = keyValue;
    }
    if(ckt.num_key_inputs() == slice.cktslice->num_key_inputs()) {
        for(auto it = allKeys.begin(); it != allKeys.end(); it++) {
            int keyValue = it->second;
            const std::string& keyName = it->first;
            keysFoundMap[keyName] = keyValue;
        }
    }

    std::vector<lbool> keyValues(ckt.num_key_inputs(), sat_n::l_Undef);
    for(unsigned ki=0; ki != ckt.num_key_inputs(); ki++) {
        auto pos = keysFound.find(ki);
        if(pos != keysFound.end()) {
            keyValues[ki] = (pos->second ? sat_n::l_True : sat_n::l_False);
        }
        else if(allKeys.size() != 0 && ckt.num_key_inputs() == slice.cktslice->num_key_inputs()) {
            auto mapPos = allKeys.find(ckt.key_inputs[ki]->name);
            assert(mapPos != allKeys.end());
            keyValues[ki] = (mapPos->second ? sat_n::l_True : sat_n::l_False);
        }
    }
    ckt.rewrite_keys(keyValues);
    return slice.outputs.size();
}

void solver_t::sliceAndDice(
    ckt_n::ckt_t& ckt,
    ckt_n::ckt_t& sim,
    std::vector<slice_t*>& slices
)
{
    using namespace ckt_n;

    // initialize the maps.
    std::vector< std::vector<bool> > keymap, outputmap;
    ckt.init_keymap(keymap);
    ckt.init_outputmap(outputmap);

    IloEnv env;

    int numNodeVars = ckt.num_nodes();
    int numOutputVars = ckt.num_outputs();
    int numKeyVars = ckt.num_key_inputs();
    int totalVars = numNodeVars + numOutputVars + numKeyVars;

    // std::cout << "# of outputs: " << numOutputVars << std::endl;
    // std::cout << "# of nodes: " << numNodeVars << std::endl;
    // std::cout << "# of keys: " << numKeyVars << std::endl;

    auto outVarIndex = [numNodeVars](int i) -> int
        { return numNodeVars + i; };
    auto keyVarIndex = [numNodeVars, numOutputVars](int i) -> int
        { return numNodeVars + numOutputVars + i; };


    // first create the variables.
    IloNumVarArray vars(env);
    int i;
    for(i=0; i != numNodeVars; i++) {
        char name[32];
        sprintf(name, "n%d", i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
    }
    for(; i < numNodeVars+numOutputVars; i++) {
        char name[32];
        sprintf(name, "o%d", i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
        assert(outVarIndex(i-numNodeVars) == i);
    }
    for(; i < numNodeVars+numOutputVars+numKeyVars; i++) {
        char name[32];
        sprintf(name, "k%d", i);
        vars.add(IloNumVar(env, 0, 1, ILOINT, name));
        assert(keyVarIndex(i-numNodeVars-numOutputVars) == i);
    }
    assert(i == totalVars);

    // and then the constraints.
    IloRangeArray cnst(env);
    int ccPos=0; // current constraint position.

    // for each key ki: ki => oj for each ouput j that is affected bby ki
    // ki => oj translates to -ki + oj >= 0
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        std::vector<int> output_indices;
        ckt.get_indices_in_bitmap(outputmap, ckt.key_inputs[i], output_indices);
        for(auto it = output_indices.begin(); it != output_indices.end(); it++) {
            unsigned j=*it;
            int vi = keyVarIndex(i);
            int vj = outVarIndex(j);
            cnst.add(IloRange(env, 0, IloInfinity));
            cnst[ccPos].setLinearCoef(vars[vi], -1);
            cnst[ccPos].setLinearCoef(vars[vj], 1);
            ccPos += 1;
        }
    }
    // for each output oi, and each node nj in the fanin cone of that output:
    // oi => nj <=> -oi + nj >= 0
    for(unsigned i=0; i != ckt.num_outputs(); i++) {
        std::vector<bool> fanin(ckt.num_nodes());
        ckt.compute_transitive_fanin(ckt.outputs[i], fanin);

        for(int j=0; j != (int)fanin.size(); j++) {
            if(fanin[j]) {
                node_t* nj = ckt.nodes[j];
                assert(nj->get_index() == j);
                int vi = outVarIndex(i);
                int vj = j;
                cnst.add(IloRange(env, 0, IloInfinity));
                cnst[ccPos].setLinearCoef(vars[vi], -1);
                cnst[ccPos].setLinearCoef(vars[vj], 1);
                ccPos += 1;
            }
        }
    }
    // now we need to create the objective function
    // minimize: number of selected nodes.
    IloObjective obj = IloMinimize(env);
    IloNumArray coeffs(env, totalVars);
    for(int i=0; i != numNodeVars; i++) {
        coeffs[i] = 1;
    }
    obj.setLinearCoefs(vars, coeffs);

    std::set<int> notYetSelectedKeys;
    for(int i=0; i != numKeyVars; i++) {
        notYetSelectedKeys.insert(i);
    }

    cnst.add(IloRange(env, 1, IloInfinity));
    int keyCnstPos = ccPos++;
    // k1 + k2 + ... + kn >= 1
    while(notYetSelectedKeys.size() > 0) {
        for(int i=0; i != numKeyVars; i++) {
            if(notYetSelectedKeys.find(i) != notYetSelectedKeys.end()) {
                int varNum = keyVarIndex(i);
                cnst[keyCnstPos].setLinearCoef(vars[varNum], 1);
            } else {
                int varNum = keyVarIndex(i);
                cnst[keyCnstPos].setLinearCoef(vars[varNum], 0);
            }
        }

        IloModel model(env);
        model.add(obj);
        model.add(cnst);
        IloCplex cplex(model);
        cplex.setOut(env.getNullStream());
        // cplex.exportModel("model.lp");
        if(!cplex.solve()) {
            std::cerr << "Error: unable to solve cplex model." << std::endl;
            exit(1);
        }
        // std::cout << "status: " << cplex.getStatus() << std::endl;
        // std::cout << "objval: " << cplex.getObjValue() << std::endl;
        IloNumArray vals(env);
        cplex.getValues(vals, vars);

        slice_t *slice = new slice_t(ckt, sim);
        for(int i=0; i != numOutputVars; i++) {
            int index = outVarIndex(i);
            int vi = (int) vals[index];
            if(vi) {
                slice->outputs.push_back(i);
            }
        }
        for(int i =0; i != numKeyVars; i++) {
            int index = keyVarIndex(i);
            int vi = (int) vals[index];
            if(vi) {
                slice->keys.push_back(i);
                notYetSelectedKeys.erase(i);
            }
        }

        std::sort(slice->outputs.begin(), slice->outputs.end());
        std::sort(slice->keys.begin(), slice->keys.end());

        slices.push_back(slice);
    }
}

void solver_t::slice_t::createCkts()
{
    using namespace ckt_n;
    assert(sim.num_outputs() == ckt.num_outputs());

    // create output list.
    nodelist_t cktOutputs, simOutputs;
    for(unsigned i=0; i != outputs.size(); i++) {
        int index = outputs[i];
        assert(index >= 0 && index < (int) ckt.num_outputs());

        node_t* out_i = ckt.outputs[index];
        cktOutputs.push_back(out_i);

        node_t* out_j = sim.outputs[index];
        simOutputs.push_back(out_j);
    }

    // now construct slice.
    cktslice = new ckt_t(ckt, cktOutputs, ckt_nmfwd, ckt_nmrev);
    simslice = new ckt_t(sim, simOutputs, sim_nmfwd, sim_nmrev);
    // make sure the inputs and outputs match up.
    if(!cktslice->compareIOs(*simslice, 1)) {
        std::cerr << "CKT SLICE" << std::endl << cktslice << std::endl;
        std::cerr << "SIM SLICE" << std::endl << simslice << std::endl;
        std::cerr << "Error. Slices are not comparable." << std::endl;
        exit(1);
    }

    //std::cout << "# of nodes: " << ckt.nodes.size() << std::endl;
    //std::cout << "# of fwd mapped nodes: " << ckt_nmfwd.size() << std::endl;

    // create the maps between the key inputs in the original and new circuit.
    // key input map.
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        node_t* ki = ckt.key_inputs[i];
        auto pos = ckt_nmfwd.find(ki);
        if(pos != ckt_nmfwd.end()) {
            node_t* kj = pos->second;
            int index = cktslice->get_key_input_index(kj);
            assert(index != -1);
            cktm_kfwd[i] = index;
            cktm_krev[index] = i;
        }
    }

    // ckt input map.
    for(unsigned i=0; i != ckt.num_ckt_inputs(); i++) {
        node_t* ci = ckt.ckt_inputs[i];
        auto pos = ckt_nmfwd.find(ci);
        if(pos != ckt_nmfwd.end()) {
            node_t* cj = pos->second;
            int index = cktslice->get_ckt_input_index(cj);
            assert(index != -1);
            cktm_cfwd[i] = index;
            cktm_crev[index] = i;
        }
    }
}

void solver_t::solveSlice(
    slice_t& slice,
    std::map<int, int>& fixedKeys,
    rmap_t& allKeys
)
{
    // first create the slice that is requested.
    using namespace ckt_n;

    assert(slice.sim.num_outputs() == slice.ckt.num_outputs());
    slice.createCkts();

    //std::cout << "slice: outputs=" << slice.cktslice->num_outputs()
    //          << "; ckt_inputs=" << slice.cktslice->num_ckt_inputs()
    //          << "; key_inputs=" << slice.cktslice->num_key_inputs()
    //          << "; gates=" << slice.cktslice->num_gates() << std::endl;

    // std::cout << *slice.cktslice << std::endl;
    // std::cout << *slice.simslice << std::endl;

    // make sure we don't have any known keys.
    // (the caller must've simplified them away).
    for(auto it=fixedKeys.begin();it != fixedKeys.end(); it++) {
        int i1 = it->first;
        int i2 = slice.mapKeyIndexFwd(i1);
        assert(i2 == -1);
    }

    // actual solving.
    solver_t S(*slice.cktslice, *slice.simslice, 0);
    S.MAX_VERIF_ITER = 1;

    S.time_limit = 60;
    getrusage(RUSAGE_SELF, &S.ru_start);
    bool finished = S.solve(solver_t::SOLVER_V0, allKeys, true);

    if(true||finished) {
        assert(slice.ckt.num_key_inputs() >= slice.cktslice->num_key_inputs());
        if(slice.ckt.num_outputs() > slice.cktslice->num_outputs()) {
            std::map<int, int> sliceKeys;
            S.findFixedKeys(sliceKeys);

            // now translate the keys back to big node.
            for(auto it=sliceKeys.begin(); it != sliceKeys.end(); it++) {
                int i1 = it->first;
                int i2 = slice.mapKeyIndexRev(i1);
                fixedKeys[i2] = it->second;
            }
        }
    }
}

void solver_t::findFixedKeys(std::map<int, int>& backbones)
{
    using namespace ckt_n;
    using namespace sat_n;

    if(iovectors.size() == 0) return;

    Solver Sckt;
    AllSAT::ClauseList cktCl;
    index2lit_map_t cktmap;
    std::vector<bool> keyinputflags;

    ckt.init_solver(Sckt, cktCl, cktmap, true /* don't care. */);
    keyinputflags.resize(Sckt.nVars(), false);
    ckt.init_keyinput_map(cktmap, keyinputflags);

    std::vector<lbool> values(Sckt.nVars(), sat_n::l_Undef);

    for(unsigned i=0; i != iovectors.size(); i++) {
        const std::vector<bool>& inputs = iovectors[i].inputs;
        const std::vector<bool>& outputs = iovectors[i].outputs;

        for(unsigned i=0; i != inputs.size(); i++) {
            int idx = ckt.ckt_inputs[i]->get_index();
            values[var(cktmap[idx])] = inputs[i] ? sat_n::l_True : sat_n::l_False;
        }

        for(unsigned i=0; i != outputs.size(); i++) {
            int idx = ckt.outputs[i]->get_index();
            values[var(cktmap[idx])] = outputs[i] ? sat_n::l_True : sat_n::l_False;
        }
        cktCl.addRewrittenClauses(values, keyinputflags, Sckt);
    }
    // now freeze the ckt inputs.
    for(unsigned i=0; i != ckt.num_ckt_inputs(); i++) {
        Lit li = cktmap[ckt.ckt_inputs[i]->get_index()];
        Sckt.freeze(li);
    }
    // and then freeze the key inputs.
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        Lit li = cktmap[ckt.key_inputs[i]->get_index()];
        Sckt.freeze(li);
    }

    // get an assignment for the keys.
    std::cout << "finding initial assignment of keys." << std::endl;
    bool result = Sckt.solve();
    assert(result);
    std::vector<Lit> keys;
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        int idx = ckt.key_inputs[i]->get_index();
        lbool value = Sckt.modelValue(var(lmap[idx]));
        keys.push_back(value == sat_n::l_True ? lmap[idx] : ~lmap[idx]);
    }
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        //std::cout << "key: " << i << std::endl;
        if(Sckt.solve(~keys[i]) == false) {
            // can't satisfy these I/O combinations with this key value.
            if(sign(keys[i])) {
                backbones[i] = 0;
            } else {
                backbones[i] = 1;
            }
            Sckt.addClause(keys[i]);
        }
    }

#if 0
    for(unsigned i=0; i != iovectors.size(); i++) {
        //std::cout << "iovector: " << i << std::endl;

        const std::vector<bool>& inputs = iovectors[i].inputs;
        _testBackbones(inputs, Sckt, cktmap, backbones);
    }
#endif
    //std::cout << "# of backbones found: " << backbones.size() << std::endl;
}

void solver_t::_testBackbones(
    const std::vector<bool>& inputs,
    sat_n::Solver& S, ckt_n::index2lit_map_t& lmap,
    std::map<int, int>& backbones)
{
#if 0
    using namespace sat_n;
    using namespace ckt_n;

    assert(inputs.size() == ckt.num_ckt_inputs());

    vec_lit_t assumps;
    for(unsigned i=0; i != inputs.size(); i++) {
        int idx = ckt.ckt_inputs[i]->get_index();
        assumps.push(inputs[i] ? lmap[idx] : ~lmap[idx]);
    }

    assert(assumps.size() == ckt.num_ckt_inputs());
    assumps.growTo(ckt.num_ckt_inputs() + 1);
    assert(assumps.size() == ckt.num_ckt_inputs()+1);

    int last = assumps.size()-1;
    for(unsigned i=0; i != ckt.num_key_inputs(); i++) {
        assumps[last] = ~keys[i];
        //std::cout << "key: " << i << std::endl;
        if(S.solve(assumps) == false) {
            // can't satisfy these I/O combinations with this key value.
            if(sign(keys[i])) {
                backbones[i] = 0;
            } else {
                backbones[i] = 1;
            }
            S.addClause(keys[i]);
        }
    }
#endif
}

