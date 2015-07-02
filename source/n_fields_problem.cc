#include "n_fields_problem.h"
#include <deal.II/lac/trilinos_precondition.h>
#include <deal.II/numerics/solution_transfer.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/function.h>
#include <deal.II/base/utilities.h>
// #include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/work_stream.h>
#include <deal.II/fe/mapping_q_eulerian.h>

#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/block_sparsity_pattern.h>
#include <deal.II/lac/trilinos_block_vector.h>
#include <deal.II/lac/trilinos_sparse_matrix.h>
//
#include <deal.II/lac/trilinos_solver.h>
#include <deal.II/lac/block_linear_operator.h>
#include <deal.II/lac/linear_operator.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/filtered_iterator.h>
#include <deal.II/grid/tria_boundary_lib.h>
#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/grid_refinement.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/solution_transfer.h>

#include <deal.II/distributed/solution_transfer.h>
// #include <deal.II/base/index_set.h>
#include <deal.II/distributed/tria.h>
// #include <deal.II/distributed/grid_refinement.h>

#include <typeinfo>
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>
#include <numeric>
#include <locale>
#include <string>
#include <math.h>

#include "equation_data.h"

using namespace dealii;

/* ------------------------ PARAMETERS ------------------------ */

template <int dim, int spacedim, int n_components>
void
NFieldsProblem<dim, spacedim, n_components>::
declare_parameters (ParameterHandler &prm)
{
  add_parameter(  prm,
                  &initial_global_refinement,
                  "Initial global refinement",
                  "1",
                  Patterns::Integer (0));

  add_parameter(  prm,
                  &n_cycles,
                  "Number of cycles",
                  "3",
                  Patterns::Integer (0));

  add_parameter(  prm,
                  &max_time_iterations,
                  "Maximum number of time steps",
                  "10000",
                  Patterns::Integer (0));


}

/* ------------------------ CONSTRUCTORS ------------------------ */

template <int dim, int spacedim, int n_components>
NFieldsProblem<dim, spacedim, n_components>::NFieldsProblem (const Interface<dim, spacedim, n_components> &energy,
    const MPI_Comm &communicator)
  :
  OdeArgument<VEC>(communicator),
  comm(communicator),
  energy(energy),
  pcout (std::cout,
         (Utilities::MPI::this_mpi_process(comm)
          == 0)),
  tcout (timer_outfile,
         (Utilities::MPI::this_mpi_process(comm)
          == 0)),
  computing_timer (comm,
                   tcout,
                   TimerOutput::summary,
                   TimerOutput::wall_times),

  eh("Error Tables", energy.get_component_names(),
     print(std::vector<std::string>(n_components, "L2,H1"), ";")),

  pgg("Domain"),

  exact_solution("Exact solution"),

  data_out("Output Parameters", "vtu"),
  dae(*this)

{}


/* ------------------------ DEGREE OF FREEDOM ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::setup_dofs ()
{
  computing_timer.enter_section("Setup dof systems");
  std::vector<unsigned int> sub_blocks = energy.get_component_blocks();
  dof_handler->distribute_dofs (*fe);
  DoFRenumbering::component_wise (*dof_handler, sub_blocks);

  mapping = energy.get_mapping(*dof_handler, solution);

  std::vector<types::global_dof_index> dofs_per_block (energy.n_blocks());
  DoFTools::count_dofs_per_block (*dof_handler, dofs_per_block,
                                  sub_blocks);

  std::locale s = pcout.get_stream().getloc();
  pcout.get_stream().imbue(std::locale(""));
  pcout << "Number of active cells: "
        << triangulation->n_global_active_cells()
        << " (on "
        << triangulation->n_levels()
        << " levels)"
        << std::endl
        << "Number of degrees of freedom: "
        << dof_handler->n_dofs()
        << "(" << print(dofs_per_block,"+") << ")"
        << std::endl
        << std::endl;
  pcout.get_stream().imbue(s);


  std::vector<IndexSet> partitioning, relevant_partitioning;
  IndexSet relevant_set;
  {
    IndexSet index_set = dof_handler->locally_owned_dofs();
    for (unsigned int i=0; i<energy.n_blocks(); ++i)
      partitioning.push_back(index_set.get_view( std::accumulate(dofs_per_block.begin(), dofs_per_block.begin()+i, 0),
                                                 std::accumulate(dofs_per_block.begin(), dofs_per_block.begin()+i+1, 0)));

    DoFTools::extract_locally_relevant_dofs (*dof_handler,
                                             relevant_set);

    for (unsigned int i=0; i<energy.n_blocks(); ++i)
      relevant_partitioning.push_back(relevant_set.get_view(std::accumulate(dofs_per_block.begin(), dofs_per_block.begin()+i, 0),
                                                            std::accumulate(dofs_per_block.begin(), dofs_per_block.begin()+i+1, 0)));
  }
  constraints.clear ();
  constraints.reinit (relevant_set);

  DoFTools::make_hanging_node_constraints (*dof_handler,
                                           constraints);

  energy.apply_bcs(*dof_handler, *fe ,constraints);

  constraints.close ();

  setup_jacobian (partitioning, relevant_partitioning);
  setup_jacobian_preconditioner (partitioning, relevant_partitioning);

  //    solution.reinit (partitioning, relevant_partitioning, MPI_COMM_WORLD);
  //    solution_dot.reinit (partitioning, relevant_partitioning, MPI_COMM_WORLD);
  //
  solution.reinit (partitioning, MPI_COMM_WORLD);
  solution_dot.reinit (partitioning, MPI_COMM_WORLD);

  computing_timer.exit_section();
}

/* ------------------------ SETUP MATRIX ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::
setup_jacobian (const std::vector<IndexSet> &partitioning,
                const std::vector<IndexSet> &relevant_partitioning)
{
  jacobian_matrix.clear ();

  TrilinosWrappers::BlockSparsityPattern sp(partitioning, partitioning,
                                            relevant_partitioning,
                                            MPI_COMM_WORLD);

  Table<2,DoFTools::Coupling> coupling = energy.get_coupling();

  DoFTools::make_sparsity_pattern (*dof_handler,
                                   coupling, sp,
                                   constraints, false,
                                   Utilities::MPI::
                                   this_mpi_process(MPI_COMM_WORLD));
  sp.compress();

  jacobian_matrix.reinit (sp);
}

/* ------------------------ PRECONDITIONER ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::
setup_jacobian_preconditioner (const std::vector<IndexSet> &partitioning,
                               const std::vector<IndexSet> &relevant_partitioning)
{
  jacobian_preconditioner_matrix.clear ();

  TrilinosWrappers::BlockSparsityPattern sp(partitioning, partitioning,
                                            relevant_partitioning,
                                            MPI_COMM_WORLD);

  Table<2,DoFTools::Coupling> coupling = energy.get_preconditioner_coupling();

  DoFTools::make_sparsity_pattern (*dof_handler,
                                   coupling, sp,
                                   constraints, false,
                                   Utilities::MPI::
                                   this_mpi_process(MPI_COMM_WORLD));
  sp.compress();

  jacobian_preconditioner_matrix.reinit (sp);
}


template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::assemble_jacobian (const double t,
    const VEC &solution,
    const VEC &solution_dot,
    const double alpha)
{
  computing_timer.enter_section ("   Assemble system");

  jacobian_matrix = 0;

  const QGauss<dim> quadrature_formula(fe->degree+1);
  const QGauss<dim-1> face_quadrature_formula(fe->degree+1);
  SAKData system_data;
  std::vector<const TrilinosWrappers::MPI::BlockVector *> sols;
  sols.push_back(&solution);
  sols.push_back(&solution_dot);

  energy.initialize_data(fe->dofs_per_cell,
                         quadrature_formula.size(),
                         face_quadrature_formula.size(),
                         solution, solution_dot, t, alpha, system_data);


  auto local_copy = [ this ]
                    (const SystemCopyData &data)
  {
    this->constraints.distribute_local_to_global (data.local_matrix,
                                                  data.local_dof_indices,
                                                  this->jacobian_matrix);
  };

  auto local_assemble = [ this ]
                        (const typename DoFHandler<dim,spacedim>::active_cell_iterator &cell,
                         Scratch &scratch,
                         SystemCopyData &data)
  {
    this->energy.assemble_local_system(cell, scratch, data);
  };

  typedef
  FilteredIterator<typename DoFHandler<dim,spacedim>::active_cell_iterator>
  CellFilter;
  WorkStream::
  run (CellFilter (IteratorFilters::LocallyOwnedCell(),
                   dof_handler->begin_active()),
       CellFilter (IteratorFilters::LocallyOwnedCell(),
                   dof_handler->end()),
       local_assemble,
       local_copy,
       Assembly::Scratch::
       NFields<dim,spacedim> (system_data,
                              *fe,
                              quadrature_formula,
                              *mapping,
                              energy.get_jacobian_flags(),
                              face_quadrature_formula,
                              energy.get_face_flags()),
       Assembly::CopyData::
       NFieldsSystem<dim,spacedim> (*fe));

  jacobian_matrix.compress(VectorOperation::add);

  pcout << std::endl;
  computing_timer.exit_section();
}




template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::assemble_jacobian_preconditioner (const double t,
    const VEC &solution,
    const VEC &solution_dot,
    const double alpha)
{
  if (energy.get_jacobian_preconditioner_flags() != update_default)
    {
      computing_timer.enter_section ("   Build preconditioner");
      pcout << "   Rebuilding preconditioner..." << std::flush;

      jacobian_preconditioner_matrix = 0;

      const QGauss<dim> quadrature_formula(fe->degree+1);
      const QGauss<dim-1> face_quadrature_formula(fe->degree+1);

      typedef
      FilteredIterator<typename DoFHandler<dim,spacedim>::active_cell_iterator>
      CellFilter;

      SAKData preconditioner_data;

      std::vector<const TrilinosWrappers::MPI::BlockVector *> sols;
      sols.push_back(&solution);
      energy.initialize_data(fe->dofs_per_cell,
                             quadrature_formula.size(),
                             face_quadrature_formula.size(),
                             solution, solution_dot, t, alpha,
                             preconditioner_data);


      auto local_copy = [this]
                        (const PreconditionerCopyData &data)
      {
        this->constraints.distribute_local_to_global (data.local_matrix,
                                                      data.local_dof_indices,
                                                      this->jacobian_preconditioner_matrix);
      };

      auto local_assemble = [ this ]
                            (const typename DoFHandler<dim,spacedim>::active_cell_iterator &cell,
                             Assembly::Scratch::NFields<dim,spacedim> &scratch,
                             PreconditionerCopyData &data)
      {
        this->energy.assemble_local_preconditioner(cell, scratch, data);
      };



      WorkStream::
      run (CellFilter (IteratorFilters::LocallyOwnedCell(),
                       dof_handler->begin_active()),
           CellFilter (IteratorFilters::LocallyOwnedCell(),
                       dof_handler->end()),
           local_assemble,
           local_copy,
           Assembly::Scratch::
           NFields<dim,spacedim> (preconditioner_data,
                                  *fe, quadrature_formula,
                                  *mapping,
                                  energy.get_jacobian_preconditioner_flags(),
                                  face_quadrature_formula,
                                  UpdateFlags(0)),
           Assembly::CopyData::
           NFieldsPreconditioner<dim,spacedim> (*fe));

      jacobian_preconditioner_matrix.compress(VectorOperation::add);

      pcout << std::endl;
      computing_timer.exit_section();
    }
}

/* ------------------------ SOLVE ------------------------ */
//
//template <int dim, int spacedim, int n_components>
//void NFieldsProblem<dim, spacedim, n_components>::solve ()
//{
//
//  pcout << "   Solving system... " << std::flush;
////
////  TrilinosWrappers::MPI::BlockVector
////  distributed_solution (rhs);
////  distributed_solution = solution;
//
//  // [TODO] make n_block independent
////  const unsigned int
////  start = (distributed_solution.block(0).size() +
////           distributed_solution.block(1).local_range().first);
////  const unsigned int
////  end   = (distributed_solution.block(0).size() +
////           distributed_solution.block(1).local_range().second);
////  for (unsigned int i=start; i<end; ++i)
////    if (constraints.is_constrained (i))
////      distributed_solution(i) = 0;
//
//  unsigned int n_iterations = 0;
//  const double solver_tolerance = 1e-8;
//
//  energy.compute_system_operators(*dof_handler,
//                                  matrix, preconditioner_matrix,
//                                  system_op, preconditioner_op);
//
//  PrimitiveVectorMemory<TrilinosWrappers::MPI::BlockVector> mem;
//  SolverControl solver_control (30, solver_tolerance);
//  SolverControl solver_control_refined (matrix.m(), solver_tolerance);
//
//  SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
//  solver(solver_control, mem,
//         SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
//         AdditionalData(30, true));
//
//  SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
//  solver_refined(solver_control_refined, mem,
//                 SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
//                 AdditionalData(50, true));
//
//  auto S_inv         = inverse_operator(system_op, solver, preconditioner_op);
//  auto S_inv_refined = inverse_operator(system_op, solver_refined, preconditioner_op);
//  try
//    {
//      S_inv.vmult(distributed_solution, rhs);
//      n_iterations = solver_control.last_step();
//    }
//  catch ( SolverControl::NoConvergence )
//    {
//      S_inv_refined.vmult(distributed_solution, rhs);
//      n_iterations = (solver_control.last_step() +
//                      solver_control_refined.last_step());
//    }
//
//
//  constraints.distribute (distributed_solution);
//  newton_update = distributed_solution;
//  TrilinosWrappers::MPI::BlockVector nwt (solution);
//  nwt = distributed_solution;
//  nwt *= fixed_alpha;
//  solution += nwt;
//
//
//  pcout << std::endl;
//  pcout << " iterations:                           " <<  n_iterations
//        << std::endl;
//  pcout << std::endl;
//
//}
/* ------------------------ residual ----------------------- */

//template <int dim, int spacedim, int n_components>
//double NFieldsProblem<dim, spacedim, n_components>::compute_residual(const double alpha)// const
//{
//  TrilinosWrappers::MPI::BlockVector evaluation_point (solution);
//  TrilinosWrappers::MPI::BlockVector nwt (solution);
//  nwt = newton_update;
//  if (alpha >1e-10)
//    {
//      nwt *= alpha;
//      evaluation_point += nwt;
//    }
//
//  const QGauss<dim> quadrature_formula(fe->degree+1);
//  const QGauss<dim-1> face_quadrature_formula(fe->degree+1);
//  SAKData residual_data;
//  std::vector<const TrilinosWrappers::MPI::BlockVector *> sols;
//  sols.push_back(&evaluation_point);
//  energy.initialize_data(fe->dofs_per_cell,
//                         quadrature_formula.size(),
//                         face_quadrature_formula.size(),
//                         sols, residual_data);
//
//  rhs=0;
//  typedef
//  FilteredIterator<typename DoFHandler<dim,spacedim>::active_cell_iterator>
//  CellFilter;
//  WorkStream::
//  run (CellFilter (IteratorFilters::LocallyOwnedCell(),
//                   dof_handler->begin_active()),
//       CellFilter (IteratorFilters::LocallyOwnedCell(),
//                   dof_handler->end()),
//       std_cxx11::bind (&NFieldsProblem<dim, spacedim, n_components>::
//                        local_assemble_system,
//                        this,
//                        std_cxx11::_1,
//                        std_cxx11::_2,
//                        std_cxx11::_3),
//       std_cxx11::bind (&NFieldsProblem<dim, spacedim, n_components>::
//                        copy_local_to_global_system,
//                        this,
//                        std_cxx11::_1),
//       Assembly::Scratch::
//       NFields<dim,spacedim> (residual_data,
//                              *fe,
//                              quadrature_formula,
//                              mapping,
//                              update_values    |
//                              update_quadrature_points  |
//                              update_JxW_values |
//                              update_gradients,
//                              face_quadrature_formula,
//                              update_values            |
//                              update_quadrature_points |
//                              update_JxW_values),
//       Assembly::CopyData::
//       NFieldsSystem<dim,spacedim> (*fe));
//
//  rhs.compress(VectorOperation::add);
//
//  return rhs.l2_norm();
//
//}
//
//template <int dim, int spacedim, int n_components>
//double NFieldsProblem<dim, spacedim, n_components>::determine_step_length() const
//{
//  return 1.0;
//}
//
/* ------------------------ MESH AND GRID ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::refine_mesh ()
{
  triangulation->refine_global (1);
  //dof_handler->distribute_dofs(*fe);
  //TrilinosWrappers::MPI::BlockVector tmp(dof_handler->n_dofs());
  //solution_transfer.interpolate(solution, tmp);
}


template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::make_grid_fe()
{
  triangulation = SP(pgg.distributed(MPI_COMM_WORLD));
  dof_handler = SP(new DoFHandler<dim,spacedim>(*triangulation));
  fe=SP(energy());
  triangulation->refine_global (initial_global_refinement);
}

/* ------------------------ ERRORS ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::process_solution ()
{
  eh.error_from_exact(*dof_handler, solution, exact_solution);
  eh.output_table(pcout);
}

/* ------------------------ RUN ------------------------ */

template <int dim, int spacedim, int n_components>
void NFieldsProblem<dim, spacedim, n_components>::run ()
{
  make_grid_fe();
  setup_dofs();

  dae.start_ode(solution, solution_dot, max_time_iterations);

  //
  //  for (unsigned int cycle=0; cycle<n_cycles; ++cycle)
  //    {
  //      if (cycle == 0)
  //        {
  //          make_grid_fe ();
  //        }
  //      else
  //        refine_mesh ();
  //
  //      double residual = 10.;
  //      setup_dofs ();
  //
  //      pcout << "Initial residual: "
  //            << compute_residual(0.0)
  //            << std::endl;
  //      unsigned int it = 0;
  //
  //      while (residual > 1e-6 && it < max_newton_it)
  //        {
  //          it += 1;
  //          assemble_system ();
  //          build_preconditioner ();
  //          solve ();
  //          residual = compute_residual(0.0);
  //          pcout << "Residual=           "
  //                << residual
  //                << std::endl;
  //          //          output_results ();
  //          process_solution ();
  //        }
  //    }

  // std::ofstream f("errors.txt");
  computing_timer.print_summary();
  timer_outfile.close();
  // f.close();
}

/*** ODE Argument Interface ***/

template <int dim, int spacedim, int n_components>
shared_ptr<VEC>
NFieldsProblem<dim, spacedim, n_components>::create_new_vector() const
{
  shared_ptr<VEC> ret = SP(new VEC(solution));
  return ret;
}


template <int dim, int spacedim, int n_components>
unsigned int
NFieldsProblem<dim, spacedim, n_components>::n_dofs() const
{
  return dof_handler->n_dofs();
}


template <int dim, int spacedim, int n_components>
void
NFieldsProblem<dim, spacedim, n_components>::output_step(const double /* t */,
                                                         const VEC &solution,
                                                         const VEC &solution_dot,                           const unsigned int step_number,
                                                         const double /* h */ )
{
  computing_timer.enter_section ("Postprocessing");

  std::stringstream suffix;
  suffix << "." << step_number;
  data_out.prepare_data_output( *dof_handler,
                                suffix.str());
  data_out.add_data_vector (solution, energy.get_component_names());
  std::vector<std::string> sol_dot_names =
    Utilities::split_string_list( energy.get_component_names());
  for (auto &name : sol_dot_names)
    {
      name += "_dot";
    }
  data_out.add_data_vector (solution_dot, print(sol_dot_names,","));


  //MappingQEulerian<dim,TrilinosWrappers::MPI::BlockVector> q_mapping(fe->degree, *dof_handler, solution);
  data_out.write_data_and_clear("",*mapping);
  // data_out.write_data_and_clear();

  computing_timer.exit_section ();
}



template <int dim, int spacedim, int n_components>
bool
NFieldsProblem<dim, spacedim, n_components>::solver_should_restart(const double t,
    const VEC &solution,
    const VEC &solution_dot,
    const unsigned int step_number,
    const double h)
{
  return false;
}


template <int dim, int spacedim, int n_components>
int
NFieldsProblem<dim, spacedim, n_components>::residual(const double t,
                                                      const VEC &solution,
                                                      const VEC &solution_dot,
                                                      VEC &dst) const
{
  const QGauss<dim> quadrature_formula(fe->degree+1);
  const QGauss<dim-1> face_quadrature_formula(fe->degree+1);

  SAKData residual_data;

  energy.initialize_data(fe->dofs_per_cell,
                         quadrature_formula.size(),
                         face_quadrature_formula.size(),
                         solution, solution_dot, t, 0.0,
                         residual_data);

  dst = 0;

  auto local_copy = [&dst, this] (const SystemCopyData &data)
  {
    this->constraints.distribute_local_to_global (data.local_rhs,
                                                  data.local_dof_indices,
                                                  dst);
  };

  auto local_assemble = [ this ]
                        (const typename DoFHandler<dim,spacedim>::active_cell_iterator &cell,
                         Assembly::Scratch::NFields<dim,spacedim> &scratch,
                         SystemCopyData &data)
  {
    const unsigned int dofs_per_cell = scratch.fe_values.get_fe().dofs_per_cell;
    scratch.fe_values.reinit (cell);
    cell->get_dof_indices (data.local_dof_indices);

    data.local_rhs = 0;
    this->energy.get_system_residual(cell, scratch, data, data.double_residual);

    for (unsigned int i=0; i<dofs_per_cell; ++i)
      data.local_rhs(i) -= data.double_residual[i];
  };

  typedef
  FilteredIterator<typename DoFHandler<dim,spacedim>::active_cell_iterator>
  CellFilter;
  WorkStream::
  run (CellFilter (IteratorFilters::LocallyOwnedCell(),
                   dof_handler->begin_active()),
       CellFilter (IteratorFilters::LocallyOwnedCell(),
                   dof_handler->end()),
       local_assemble,
       local_copy,
       Scratch(residual_data,
               *fe,
               quadrature_formula,
               *mapping,
               energy.get_jacobian_flags(),
               face_quadrature_formula,
               energy.get_face_flags()),
       SystemCopyData(*fe));

  dst.compress(VectorOperation::add);
  return 0;
}


template <int dim, int spacedim, int n_components>
void
NFieldsProblem<dim, spacedim, n_components>::jacobian(VEC &dst, const VEC &src) const
{
  jacobian_op.vmult(dst, src);
}



template <int dim, int spacedim, int n_components>
void
NFieldsProblem<dim, spacedim, n_components>::solve_jacobian_system(VEC &dst, const VEC &src, const double tol) const
{

  pcout << "   Solving system... " << std::flush;
  //
  //  TrilinosWrappers::MPI::BlockVector
  //  distributed_solution (rhs);
  //  distributed_solution = solution;

  // [TODO] make n_block independent
  //  const unsigned int
  //  start = (distributed_solution.block(0).size() +
  //           distributed_solution.block(1).local_range().first);
  //  const unsigned int
  //  end   = (distributed_solution.block(0).size() +
  //           distributed_solution.block(1).local_range().second);
  //  for (unsigned int i=start; i<end; ++i)
  //    if (constraints.is_constrained (i))
  //      distributed_solution(i) = 0;

  unsigned int n_iterations = 0;
  const double solver_tolerance = tol;

  PrimitiveVectorMemory<TrilinosWrappers::MPI::BlockVector> mem;
  SolverControl solver_control (30, solver_tolerance);
  SolverControl solver_control_refined (jacobian_matrix.m(), solver_tolerance);

  SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
  solver(solver_control, mem,
         SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
         AdditionalData(30, true));

  SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
  solver_refined(solver_control_refined, mem,
                 SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
                 AdditionalData(50, true));

  auto S_inv         = inverse_operator(jacobian_op, solver, jacobian_preconditioner_op);
  auto S_inv_refined = inverse_operator(jacobian_op, solver_refined, jacobian_preconditioner_op);
  try
    {
      S_inv.vmult(dst, src);
      n_iterations = solver_control.last_step();
    }
  catch ( SolverControl::NoConvergence )
    {
      S_inv_refined.vmult(dst, src);
      n_iterations = (solver_control.last_step() +
                      solver_control_refined.last_step());
    }

  constraints.distribute (dst);

  pcout << std::endl;
  pcout << " iterations:                           " <<  n_iterations
        << std::endl;
  pcout << std::endl;

}


template <int dim, int spacedim, int n_components>
int
NFieldsProblem<dim, spacedim, n_components>::setup_jacobian(const double t,
                                                            const VEC &src_yy,
                                                            const VEC &src_yp,
                                                            const double alpha)
{

  assemble_jacobian(t, src_yy, src_yp, alpha);
  assemble_jacobian_preconditioner(t, src_yy, src_yp, alpha);

  energy.compute_system_operators(*dof_handler,
                                  jacobian_matrix, jacobian_preconditioner_matrix,
                                  jacobian_op, jacobian_preconditioner_op);

  return 0;
}



template <int dim, int spacedim, int n_components>
VEC &
NFieldsProblem<dim, spacedim, n_components>::differential_components() const
{
  static VEC diff_comps;
  diff_comps.reinit(solution);
  diff_comps = 1;

  auto id = diff_comps.locally_owned_elements();
  for (unsigned int i=0; i<id.n_elements(); ++i)
    {
      auto j = id.nth_index_in_set(i);
      if (constraints.is_constrained(i))
        diff_comps[i] = 0;
    }
  return diff_comps;
}

// template class NFieldsProblem<1>;

template class NFieldsProblem<1,1,1>;
template class NFieldsProblem<1,1,2>;
template class NFieldsProblem<1,1,3>;
template class NFieldsProblem<1,1,4>;

// template class NFieldsProblem<1,2,1>;
// template class NFieldsProblem<1,2,2>;
// template class NFieldsProblem<1,2,3>;
// template class NFieldsProblem<1,2,4>;

template class NFieldsProblem<2,2,1>;
template class NFieldsProblem<2,2,2>;
template class NFieldsProblem<2,2,3>;
template class NFieldsProblem<2,2,4>;


// template class NFieldsProblem<2,3,1>;
// template class NFieldsProblem<2,3,2>;
// template class NFieldsProblem<2,3,3>;
// template class NFieldsProblem<2,3,4>;


template class NFieldsProblem<3,3,1>;
template class NFieldsProblem<3,3,2>;
template class NFieldsProblem<3,3,3>;
template class NFieldsProblem<3,3,4>;
// template class NFieldsProblem<3>;
