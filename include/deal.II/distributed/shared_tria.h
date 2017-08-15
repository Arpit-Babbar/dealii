// ---------------------------------------------------------------------
// $Id: tria.h 32739 2014-04-08 16:39:47Z denis.davydov $
//
// Copyright (C) 2008 - 2016 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE at
// the top level of the deal.II distribution.
//
// ---------------------------------------------------------------------

#ifndef dealii_distributed_shared_tria_h
#define dealii_distributed_shared_tria_h


#include <deal.II/base/config.h>
#include <deal.II/base/subscriptor.h>
#include <deal.II/base/smartpointer.h>
#include <deal.II/base/template_constraints.h>
#include <deal.II/grid/tria.h>

#include <deal.II/distributed/tria_base.h>


#include <tuple>
#include <functional>
#include <set>
#include <vector>
#include <list>
#include <utility>

#ifdef DEAL_II_WITH_MPI
#  include <mpi.h>
#endif


DEAL_II_NAMESPACE_OPEN

namespace parallel
{

#ifdef DEAL_II_WITH_MPI


  namespace shared
  {

    /**
     * This class provides a parallel triangulation for which every processor
     * knows about every cell of the global mesh (unlike for the
     * parallel::distributed::Triangulation class) but in which cells are
     * automatically partitioned when run with MPI so that each processor
     * "owns" a subset of cells. The use of this class is demonstrated in
     * step-18.
     *
     * Different from the parallel::distributed::Triangulation, this implies
     * that the entire mesh is stored on each processor. While this is clearly
     * a memory bottleneck that limits the use of this class to a few dozen
     * or hundreds of MPI processes, the partitioning of the mesh can be used
     * to partition work such as assembly or postprocessing between
     * participating processors, and it can also be used to partition which
     * processor stores which parts of matrices and vectors. As a consequence,
     * using this class is often a gentler introduction to parallelizing a
     * code than the more involved parallel::distributed::Triangulation class
     * in which processors only know their own part of the mesh, but nothing
     * about cells owned by other processors with the exception of a single
     * layer of ghost cells around their own part of the domain.
     *
     * The class is also useful in cases where compute time and memory
     * considerations dictate that the program needs to be run in parallel,
     * but where algorithmic concerns require that every processor knows
     * about the entire mesh. An example could be where an application
     * has to have both volume and surface meshes that can then both
     * be partitioned independently, but where it is difficult to ensure
     * that the locally owned set of surface mesh cells is adjacent to the
     * locally owned set of volume mesh cells and the other way around. In
     * such cases, knowing the <i>entirety</i> of both meshes ensures that
     * assembly of coupling terms can be implemented without also
     * implementing overly complicated schemes to transfer information about
     * adjacent cells from processor to processor.
     *
     * By default, the partitioning of cells between processors is done
     * automatically by calling the METIS library. By passing appropriate
     * flags to the constructor of this class (see the Triangulation::Settings
     * enum), it is possible to select other modes of partitioning the mesh,
     * including ways that are dictated by the application and not by the
     * desire to minimize the length of the interface between subdomains owned
     * by processors. Both the DoFHandler and hp::DoFHandler classes know how
     * to enumerate degrees of freedom in ways appropriate for the partitioned mesh.
     *
     * @author Denis Davydov, 2015
     * @ingroup distributed
     *
     */
    template <int dim, int spacedim = dim>
    class Triangulation : public dealii::parallel::Triangulation<dim,spacedim>
    {
    public:
      typedef typename dealii::Triangulation<dim,spacedim>::active_cell_iterator active_cell_iterator;
      typedef typename dealii::Triangulation<dim,spacedim>::cell_iterator        cell_iterator;

      /**
       * Configuration flags for distributed Triangulations to be set in the
       * constructor. Settings can be combined using bitwise OR.
       *
       * The constructor requires that exactly one of <code>partition_metis</code>,
       * <code>partition_zorder</code>, and <code>partition_custom_signal</code> be set.
       * If no setting is given to the constructor, it will set <code>partition_metis</code>
       * by default.
       */
      enum Settings
      {
        /**
         * Use METIS partitioner to partition active cells. This is the
         * default partioning method.
         */
        partition_metis = 0x1,

        /**
         * Partition active cells with the same scheme used in the
         * p4est library.
         */
        partition_zorder = 0x2,

        /**
         * Partition cells using a custom, user defined function. This is
         * accomplished by connecting the post_refinement signal to the
         * triangulation whenever it is first created and passing the user
         * defined function through the signal using <code>std::bind</code>.
         * Here is an example:
         *  @code
         *     template <int dim>
         *     void mypartition(parallel::shared::Triangulation<dim> &tria)
         *     {
         *       // user defined partitioning scheme: assign subdomain_ids
         *       // round-robin in a mostly random way:
         *       std::vector<unsigned int> assignment = {0,0,1,2,0,0,2,1,0,2,2,1,2,2,0,0};
         *       typename Triangulation<dim>::active_cell_iterator
         *         cell = tria.begin_active(),
         *         endc = tria.end();
         *       unsigned int index = 0;
         *       for (; cell != endc; ++cell, ++index)
         *         cell->set_subdomain_id(assignment[index%16]);
         *     }
         *
         *     int main ()
         *     {
         *       parallel::shared::Triangulation<dim> tria(...,
         *                                                 parallel::shared::Triangulation<dim>::Settings::partition_custom_signal);
         *       tria.signals.post_refinement.connect (std::bind(&mypartition<dim>, std::ref(tria)));
         *     }
         *  @endcode
         *
         * An equivalent code using lambda functions would look like this:
         *  @code
         *     int main ()
         *     {
         *       parallel::shared::Triangulation<dim> tria(...,
         *                                                 parallel::shared::Triangulation<dim>::Settings::partition_custom_signal);
         *       tria.signals.post_refinement.connect ([&tria]()
         *                                             {
         *                                               // user defined partitioning scheme
         *                                               // as above
         *                                               ...
         *                                             }
         *                                            );
         *     }
         *  @endcode
         *
         *
         * @note If you plan to use a custom partition with geometric multigrid,
         * you must manually partition the level cells in addition to the active cells.
         */
        partition_custom_signal = 0x4,

        /**
         * This flag needs to be set to use the geometric multigrid
         * functionality. This option requires additional computation and communication.
         *
         * Note: This flag should always be set alongside a flag for an
         * active cell partitioning method.
         */
        construct_multigrid_hierarchy = 0x8,
      };


      /**
       * Constructor.
       *
       * If @p allow_aritifical_cells is true, this class will behave similar
       * to parallel::distributed::Triangulation in that there will be locally
       * owned, ghost and artificial cells.
       *
       * Otherwise all non-locally owned cells are considered ghost.
       */
      Triangulation (MPI_Comm mpi_communicator,
                     const typename dealii::Triangulation<dim,spacedim>::MeshSmoothing =
                       (dealii::Triangulation<dim,spacedim>::none),
                     const bool allow_artificial_cells = false,
                     const Settings settings = partition_metis);

      /**
       * Destructor.
       */
      virtual ~Triangulation ();

      /**
       * Coarsen and refine the mesh according to refinement and coarsening
       * flags set.
       *
       * This step is equivalent to the dealii::Triangulation class with an
       * addition of calling dealii::GridTools::partition_triangulation() at
       * the end.
       */
      virtual void execute_coarsening_and_refinement ();

      /**
       * Create a triangulation.
       *
       * This function also partitions triangulation based on the MPI
       * communicator provided to the constructor.
       */
      virtual void create_triangulation (const std::vector< Point< spacedim > > &vertices,
                                         const std::vector< CellData< dim > > &cells,
                                         const SubCellData &subcelldata);

      /**
       * Copy @p other_tria to this triangulation.
       *
       * This function also partitions triangulation based on the MPI
       * communicator provided to the constructor.
       *
       * @note This function can not be used with parallel::distributed::Triangulation,
       * since it only stores those cells that it owns, one layer of ghost cells around
       * the ones it locally owns, and a number of artificial cells.
       */
      virtual void copy_triangulation (const dealii::Triangulation<dim, spacedim> &other_tria);

      /**
       * Read the data of this object from a stream for the purpose of
       * serialization. Throw away the previous content.
       *
       * This function first does the same work as in dealii::Triangulation::load,
       * then partitions the triangulation based on the MPI communicator
       * provided to the constructor.
       */
      template <class Archive>
      void load (Archive &ar, const unsigned int version);

      /**
       * Return a vector of length Triangulation::n_active_cells() where each
       * element stores the subdomain id of the owner of this cell. The
       * elements of the vector are obviously the same as the subdomain ids
       * for locally owned and ghost cells, but are also correct for
       * artificial cells that do not store who the owner of the cell is in
       * their subdomain_id field.
       */
      const std::vector<types::subdomain_id> &get_true_subdomain_ids_of_cells() const;

      /**
       * Return allow_artificial_cells , namely true if artificial cells are
       * allowed.
       */
      bool with_artificial_cells() const;

    private:

      /**
       * Settings
       */
      const Settings settings;

      /**
       * A flag to decide whether or not artificial cells are allowed.
       */
      const bool allow_artificial_cells;

      /**
       * This function calls GridTools::partition_triangulation () and if
       * requested in the constructor of the class marks artificial cells.
       */
      void partition();

      /**
       * A vector containing subdomain IDs of cells obtained by partitioning
       * using METIS. In case allow_artificial_cells is false, this vector is
       * consistent with IDs stored in cell->subdomain_id() of the
       * triangulation class. When allow_artificial_cells is true, cells which
       * are artificial will have cell->subdomain_id() == numbers::artificial;
       *
       * The original parition information is stored to allow using sequential
       * DoF distribution and partitioning functions with semi-artificial
       * cells.
       */
      std::vector<types::subdomain_id> true_subdomain_ids_of_cells;
    };

    template <int dim, int spacedim>
    template <class Archive>
    void
    Triangulation<dim,spacedim>::load (Archive &ar,
                                       const unsigned int version)
    {
      dealii::Triangulation<dim,spacedim>::load (ar, version);
      partition();
      this->update_number_cache ();
    }
  }
#else

  namespace shared
  {

    /**
     * Dummy class the compiler chooses for parallel shared triangulations if
     * we didn't actually configure deal.II with the MPI library. The
     * existence of this class allows us to refer to
     * parallel::shared::Triangulation objects throughout the library even if
     * it is disabled.
     *
     * Since the constructor of this class is deleted, no such objects
     * can actually be created as this would be pointless given that
     * MPI is not available.
     */
    template <int dim, int spacedim = dim>
    class Triangulation : public dealii::parallel::Triangulation<dim,spacedim>
    {
    public:
      /**
       * Constructor. Deleted to make sure that objects of this type cannot be
       * constructed (see also the class documentation).
       */
      Triangulation () = delete;

      /**
       * A dummy function to return empty vector.
       */
      const std::vector<types::subdomain_id> &get_true_subdomain_ids_of_cells() const;

      /**
       * A dummy function which always returns true.
       */
      bool with_artificial_cells() const;

    private:
      /**
       * A dummy vector.
       */
      std::vector<types::subdomain_id> true_subdomain_ids_of_cells;
    };
  }


#endif
}

DEAL_II_NAMESPACE_CLOSE

#endif
