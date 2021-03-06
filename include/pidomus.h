/**
 * Solve time-dependent non-linear n-fields problem
 * in parallel.
 *
 *
 *
 *
 *
 *
 *
 *
 *
 */

#ifndef __pi_DoMUS_h_
#define __pi_DoMUS_h_


#include <deal.II/base/timer.h>
// #include <deal.II/base/parameter_handler.h>

#include <deal.II/lac/trilinos_precondition.h>
#include <deal.II/lac/trilinos_block_sparse_matrix.h>
#include <deal.II/lac/linear_operator.h>
#include <mpi.h>

// #include <deal.II/lac/precondition.h>

#include "copy_data.h"
#include "base_interface.h"
#include <deal2lkit/parsed_grid_generator.h>
#include <deal2lkit/parsed_finite_element.h>
#include <deal2lkit/parsed_grid_refinement.h>
#include <deal2lkit/error_handler.h>
#include <deal2lkit/parsed_function.h>
#include <deal2lkit/parsed_data_out.h>
#include <deal2lkit/parameter_acceptor.h>
#include <deal2lkit/sundials_interface.h>
#include <deal2lkit/ida_interface.h>
#include <deal2lkit/imex_stepper.h>
#include <deal2lkit/parsed_zero_average_constraints.h>

#include <deal2lkit/any_data.h>
#include <deal2lkit/fe_values_cache.h>

#include "lac/lac_type.h"
#include "lac/lac_initializer.h"

using namespace dealii;
using namespace deal2lkit;
using namespace pidomus;

template <int dim, int spacedim = dim, typename LAC = LATrilinos>
class piDoMUS : public ParameterAcceptor, public SundialsInterface<typename LAC::VectorType>
{


  // This is a class required to make tests
  template<int fdim, int fspacedim, typename fn_LAC>
  friend void test(piDoMUS<fdim, fspacedim, fn_LAC> &);

public:


  piDoMUS (const std::string &name,
           const BaseInterface<dim, spacedim, LAC> &energy,
           const MPI_Comm &comm = MPI_COMM_WORLD);

  virtual void declare_parameters(ParameterHandler &prm);
  virtual void parse_parameters_call_back();

  void run ();


  /*********************************************************
   * Public interface from SundialsInterface
   *********************************************************/
  virtual shared_ptr<typename LAC::VectorType>
  create_new_vector() const;

  /** Returns the number of degrees of freedom. Pure virtual function. */
  virtual unsigned int n_dofs() const;

  /** This function is called at the end of each iteration step for
   * the ode solver. Once again, the conversion between pointers and
   * other forms of vectors need to be done inside the inheriting
   * class. */
  virtual void output_step(const double t,
                           const typename LAC::VectorType &solution,
                           const typename LAC::VectorType &solution_dot,
                           const unsigned int step_number,
                           const double h);

  /** This function will check the behaviour of the solution. If it
   * is converged or if it is becoming unstable the time integrator
   * will be stopped. If the convergence is not achived the
   * calculation will be continued. If necessary, it can also reset
   * the time stepper. */
  virtual bool solver_should_restart(const double t,
                                     const unsigned int step_number,
                                     const double h,
                                     typename LAC::VectorType &solution,
                                     typename LAC::VectorType &solution_dot);

  /** For dae problems, we need a
   residual function. */
  virtual int residual(const double t,
                       const typename LAC::VectorType &src_yy,
                       const typename LAC::VectorType &src_yp,
                       typename LAC::VectorType &dst);

  /** Setup Jacobian system and preconditioner. */
  virtual int setup_jacobian(const double t,
                             const typename LAC::VectorType &src_yy,
                             const typename LAC::VectorType &src_yp,
                             const typename LAC::VectorType &residual,
                             const double alpha);


  /** Inverse of the Jacobian vector product. */
  virtual int solve_jacobian_system(const double t,
                                    const typename LAC::VectorType &y,
                                    const typename LAC::VectorType &y_dot,
                                    const typename LAC::VectorType &residual,
                                    const double alpha,
                                    const typename LAC::VectorType &src,
                                    typename LAC::VectorType &dst) const;



  /** And an identification of the
   differential components. This
   has to be 1 if the
   corresponding variable is a
   differential component, zero
   otherwise.  */
  virtual typename LAC::VectorType &differential_components() const;

  /**
   * This function is used to get back the solution.
   */
  typename LAC::VectorType &get_solution();

private:


  /**
   * set time to @p t for forcing terms and boundary conditions
   */
  void update_functions_and_constraints(const double &t);




  /**
   * Applies Dirichlet boundary conditions
   *
   * This function is used to applies Dirichlet boundary conditions.
   * It takes as argument a DoF handler @p dof_handler, a
   * ParsedDirichletBCs and a constraint matrix @p constraints.
   *
   */
  void apply_dirichlet_bcs (const DoFHandler<dim,spacedim> &dof_handler,
                            const ParsedDirichletBCs<dim,spacedim> &bc,
                            ConstraintMatrix &constraints) const;

  /**
   * Applies Neumann boundary conditions
   *
   */
  void apply_neumann_bcs (const typename DoFHandler<dim,spacedim>::active_cell_iterator &cell,
                          FEValuesCache<dim,spacedim> &scratch,
                          std::vector<double> &local_residual) const;


  /**
   * Applies CONSERVATIVE forcing terms.
   * This function applies the conservative forcing terms, which can be
   * defined by expressions in the parameter file.
   *
   * If the problem involves NON-conservative loads, they must be included
   * in the residual formulation.
   *
   */
  void apply_forcing_terms (const typename DoFHandler<dim,spacedim>::active_cell_iterator &cell,
                            FEValuesCache<dim,spacedim> &scratch,
                            std::vector<double> &local_residual) const;



  void make_grid_fe();
  void setup_dofs (const bool &first_run = true);

  void assemble_matrices (const double t,
                          const typename LAC::VectorType &y,
                          const typename LAC::VectorType &y_dot,
                          const double alpha);
  void refine_mesh ();

  void refine_and_transfer_solutions (LADealII::VectorType &y,
                                      LADealII::VectorType &y_dot,
                                      LADealII::VectorType &y_expl,
                                      LADealII::VectorType &distributed_y,
                                      LADealII::VectorType &distributed_y_dot,
                                      LADealII::VectorType &distributed_y_expl,
                                      bool adaptive_refinement);


  void refine_and_transfer_solutions (LATrilinos::VectorType &y,
                                      LATrilinos::VectorType &y_dot,
                                      LATrilinos::VectorType &y_expl,
                                      LATrilinos::VectorType &distributed_y,
                                      LATrilinos::VectorType &distributed_y_dot,
                                      LATrilinos::VectorType &distributed_y_expl,
                                      bool adaptive_refinement);


  void set_constrained_dofs_to_zero(typename LAC::VectorType &v) const;

  const MPI_Comm &comm;
  const BaseInterface<dim, spacedim, LAC>    &interface;

  unsigned int n_cycles;
  unsigned int current_cycle;
  unsigned int initial_global_refinement;
  unsigned int max_time_iterations;

  ConditionalOStream        pcout;

  shared_ptr<parallel::distributed::Triangulation<dim, spacedim> > triangulation;
  shared_ptr<FiniteElement<dim, spacedim> >       fe;
  shared_ptr<DoFHandler<dim, spacedim> >          dof_handler;

  ConstraintMatrix                          constraints;
  ConstraintMatrix                          constraints_dot;

  LinearOperator<typename LAC::VectorType> jacobian_preconditioner_op;
  LinearOperator<typename LAC::VectorType> jacobian_op;

  typename LAC::VectorType        solution;
  typename LAC::VectorType        solution_dot;
  typename LAC::VectorType        explicit_solution;

  mutable typename LAC::VectorType        distributed_solution;
  mutable typename LAC::VectorType        distributed_solution_dot;
  mutable typename LAC::VectorType        distributed_explicit_solution;

  /**
   * Show timer statistics 
   */
  bool                     output_timer;
  /**
   * Teucos timer file
   */
  mutable TimeMonitor       computing_timer;
  
  ParsedDataOut<dim, spacedim>            data_out;

  const unsigned int n_matrices;
  std::vector<shared_ptr<typename LAC::BlockSparsityPattern> > matrix_sparsities;
  std::vector<shared_ptr<typename LAC::BlockMatrix> >  matrices;

  ErrorHandler<1>       eh;
  ParsedGridGenerator<dim, spacedim>   pgg;
  ParsedGridRefinement  pgr;

  ParsedFunction<spacedim>        exact_solution;

  ParsedFunction<spacedim>        initial_solution;
  ParsedFunction<spacedim>        initial_solution_dot;


  ParsedMappedFunctions<spacedim>  forcing_terms; // on the volume
  ParsedMappedFunctions<spacedim>  neumann_bcs;
  ParsedDirichletBCs<dim,spacedim> dirichlet_bcs;
  ParsedDirichletBCs<dim,spacedim> dirichlet_bcs_dot;

  ParsedZeroAverageConstraints<dim,spacedim> zero_average;



  IDAInterface<typename LAC::VectorType>  ida;
  IMEXStepper<typename LAC::VectorType> euler;


  std::vector<types::global_dof_index> dofs_per_block;
  IndexSet global_partitioning;
  std::vector<IndexSet> partitioning;
  std::vector<IndexSet> relevant_partitioning;

  bool adaptive_refinement;
  const bool we_are_parallel;
  bool use_direct_solver;

  /**
   * Solver tolerance for the equation:
   * \f[ \mathcal{J}(F)[u] = R[u] \f]
   */
  double jacobian_solver_tolerance;

  /**
   * Print all avaible information about processes.
   */
  bool verbose;

  /**
   * Overwrite newton's iterations: every time step shows only the last value.
   */
  bool overwrite_iter;

  /**
   * time stepper to be used
   */
  std::string time_stepper;

  /**
   * previous time step
   */
  double old_t;

  /**
   * refine mesh during transients
   */
  bool use_space_adaptivity;

  /**
   * threshold for refine mesh during transients
   */
  double kelly_threshold;

};

#endif
