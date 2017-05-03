
#include <p8est_mesh.h>
#include "ns.h"
#include "util.h"
#include "data.h"

void init(p8est_t *p8est,
          p4est_topidx_t which_tree,
          p8est_quadrant_t *quad) {
    context_t       *ctx = (context_t *) p8est->user_pointer;
    data_t          *data = (data_t *) quad->p.user_data;

    init_solver(data, ctx);

    data->boundary = -1;
    data->bx = 0;
    data->by = 0;
    data->bz = 0;
}

int refine_always (p8est_t *p8est,
                   p4est_topidx_t which_tree,
                   p8est_quadrant_t *q) {
    return 1;
}

int refine_fn (p8est_t *p8est,
               p4est_topidx_t which_tree,
               p8est_quadrant_t *q) {
    context_t *ctx = (context_t *) p8est->user_pointer;
    double midpoint[3];
    double h = (double) P4EST_QUADRANT_LEN (q->level) / (double) P4EST_ROOT_LEN;
    double l;

    get_midpoint(p8est, which_tree, q, midpoint);

    l = pow(midpoint[0] - ctx->center[0], 2.) +
        pow(midpoint[1] - ctx->center[1], 2.) +
        pow(midpoint[2] - ctx->center[2], 2.);

    if (l < ctx->width &&
        l > 0.08 &&
        h > 0.02)
        return 1;

    return 0;
}

void mesh_neighbors_iter(p8est_t *p8est,
                         p8est_ghost_t *ghost,
                         p8est_mesh_t *mesh,
                         void *ghost_data) {
    p8est_quadrant_t    *neighbor  = NULL;
    p4est_topidx_t      which_tree = -1;
    context_t           *ctx       = (context_t *) p8est->user_pointer;

    p8est_mesh_face_neighbor_t mfn;
    p4est_locidx_t      qumid, quadrant_id, which_quad;
    p8est_quadrant_t    *q;                             // current quad
    data_t              *g_data;                        // ghost data
    data_t              *data;

    int                 nface;
    int                 nrank;
    double              h;
    double              nx, ny, nz;

    // iterate for all quadrants
    for (qumid = 0; qumid < mesh->local_num_quadrants; ++qumid) {
        q = p8est_mesh_quadrant_cumulative (p8est,
                                            qumid,
                                            &which_tree,
                                            &quadrant_id);
        data = (data_t *) q->p.user_data;

        p8est_mesh_face_neighbor_init2 (&mfn,
                                        p8est,
                                        ghost,
                                        mesh,
                                        which_tree,
                                        quadrant_id);


        while ((neighbor = p8est_mesh_face_neighbor_next(&mfn,
                                                         &which_tree,
                                                         &which_quad,
                                                         &nface,
                                                         &nrank)) != NULL) {
            // get neighbor data using ghosts
            g_data = (data_t *) p8est_mesh_face_neighbor_data (&mfn, ghost_data);

            // neighbor orientation
            nx = 0;
            ny = 0;
            nz = 0;

            switch (nface) {
                case 0:                      // -x side
                    nx = -1;
                    break;
                case 1:                      // +x side
                    nx = 1;
                    break;
                case 2:                      // -y side
                    ny = -1;
                    break;
                case 3:                      // +y side
                    ny = 1;
                    break;
                case 4:                      // -z side
                    nz = -1;
                    break;
                case 5:                      // +z side
                    nz = 1;
                    break;
                default:
                    nx = ny = nz = 0;
                    break;
            }

            // detect boundary
            if (0 <= nface && nface < P8EST_FACES && is_quadrant_on_face_boundary(p8est, which_tree, nface, q)) {
                if (data->boundary == -1)
                    data->boundary = ipow(2, nface);
                else
                    data->boundary |= ipow(2, nface);

                data->bx = nx;
                data->by = ny;
                data->bz = nz;
            }

            h = (double) P8EST_QUADRANT_LEN (q->level) / (double) P8EST_ROOT_LEN;

            // TODO Calculate flow
            flow(data, nx, ny, nz);
        }
    }
}

void mesh_iter(p8est_t *p8est, p8est_mesh_t *mesh) {
    p4est_topidx_t      which_tree = -1;
    context_t           *ctx       = (context_t *) p8est->user_pointer;

    p8est_mesh_face_neighbor_t mfn;
    p4est_locidx_t      qumid, quadrant_id;
    p8est_quadrant_t    *q;                             /* current quad */
    data_t              *data;

    double              h;
    double              p[3];

    for (qumid = 0; qumid < mesh->local_num_quadrants; ++qumid) {
        q = p8est_mesh_quadrant_cumulative (p8est, qumid,
                                            &which_tree, &quadrant_id);

        /* side length */
        h = (double) P8EST_QUADRANT_LEN (q->level) / (double) P8EST_ROOT_LEN;
        get_midpoint(p8est, which_tree, q, p);

        data = (data_t *) q->p.user_data;

        // calculate CFL
        cflq(data, ctx, h);
    }
}

void solve(p8est_t *p8est,
           int step) {
    SC_PRODUCTIONF("Start solve step %d\n", step);

    int                 mpiret;
    data_t              *ghost_data;
    context_t           *ctx = (context_t *) p8est->user_pointer;
    p8est_ghost_t       *ghost;
    p8est_mesh_t        *mesh;

    /* create the ghost quadrants */
    ghost = p8est_ghost_new (p8est, P8EST_CONNECT_FACE);
    ghost_data = P4EST_ALLOC (data_t, ghost->ghosts.elem_count);
    p8est_ghost_exchange_data (p8est, ghost, ghost_data);

    mesh = p8est_mesh_new(p8est, ghost, P8EST_CONNECT_FACE);
    SC_PRODUCTIONF("Used memory: %ld\n", p8est_mesh_memory_used(mesh));

    /* calc f2 and partials */
    SC_PRODUCTION("Cell iter started\n");
    mesh_iter(p8est, mesh);
    SC_PRODUCTION("Cell iter ended\n");

    /* calc min dt */
    SC_PRODUCTIONF("dt old: %.20lf\n", ctx->dt);
    mpiret = MPI_Allreduce(MPI_IN_PLACE, &ctx->dt, 1, MPI_DOUBLE, MPI_MIN, p8est->mpicomm);
    SC_CHECK_MPI(mpiret);
    SC_PRODUCTIONF("dt new: %.20lf\n", ctx->dt);

    /* exchange ghost data */
    SC_PRODUCTION("Exchange started\n");
    p8est_ghost_exchange_data (p8est, ghost, ghost_data);
    SC_PRODUCTION("Exchange ended\n");

    SC_PRODUCTION("Neighbors iter started\n");
    mesh_neighbors_iter(p8est, ghost, mesh, ghost_data);
    SC_PRODUCTION("Neighbors iter ended\n");

    /* exchange ghost data */
    SC_PRODUCTION("Exchange started\n");
    p8est_ghost_exchange_data (p8est, ghost, ghost_data);
    SC_PRODUCTION("Exchange ended\n");

    /* generate vtk and print solution */
    write_vtk(p8est, step);

    /* clear */
    p8est_mesh_destroy(mesh);
    P4EST_FREE (ghost_data);
    p4est_ghost_destroy (ghost);

    SC_PRODUCTIONF("End solve step %d\n", step);
}

void get_boundary_data(p8est_iter_volume_info_t *info, void *user_data) {
    sc_array_t          *u_interp = (sc_array_t *) user_data;
    /* we passed the array of values to fill as the
     * user_data in the call to p4est_iterate */

    p8est_t             *p8est = info->p4est;
    p8est_quadrant_t    *q = info->quad;
    p4est_topidx_t      which_tree = info->treeid;
    p4est_locidx_t      local_id = info->quadid;
    /* this is the index of q *within its tree's numbering*.
     * We want to convert it its index for all the quadrants on this process, which we do below */

    p8est_tree_t        *tree;
    data_t              *data = (data_t *) q->p.user_data;
    p4est_locidx_t      array_offset;
    double              *this_u_ptr;
    int                 i;

    tree = p8est_tree_array_index (p8est->trees, which_tree);
    local_id += tree->quadrants_offset;             /* now the id is relative to the MPI process */
    array_offset = P8EST_CHILDREN * local_id;      /* each local quadrant has 2^d (P4EST_CHILDREN) values in u_interp */

    for (i = 0; i < P8EST_CHILDREN; i++) {
        this_u_ptr = (double *) sc_array_index (u_interp, array_offset + i);
        this_u_ptr[0] = data->boundary;
    }
}

void write_vtk(p8est_t *p8est, int step) {
    char                filename[BUFSIZ] = { '\0' };
    p4est_locidx_t      numquads;
    sc_array_t          *boundary_data;

    snprintf (filename, 12, "solution_%02d", step);
    numquads = p8est->local_num_quadrants;

    /* create a vector with one value for the corner of every local quadrant
     * (the number of children is always the same as the number of corners) */
    boundary_data = sc_array_new_size (sizeof (double), numquads * P8EST_CHILDREN);

    /* Use the iterator to visit every cell and fill in the solution values.
     * Using the iterator is not absolutely necessary in this case: we could
     * also loop over every tree (there is only one tree in this case) and loop
     * over every quadrant within every tree */
    p8est_iterate(p8est, NULL,   /* we don't need any ghost quadrants for this loop */
                  (void *) boundary_data,     /* pass in boundary so that we can fill it */
                  get_boundary_data,    /* callback function that fill boundary */
                  NULL,          /* there is no callback for the faces between quadrants */
                  NULL,          /* there is no callback for the edges between quadrants */
                  NULL);         /* there is no callback for the corners between quadrants */

    /* create VTK output context and set its parameters */
    p8est_vtk_context_t *context = p8est_vtk_context_new (p8est, filename);
    p8est_vtk_context_set_scale (context, 0.99);  /* quadrant at almost full scale */

    /* begin writing the output files */
    context = p8est_vtk_write_header (context);
    SC_CHECK_ABORT (context != NULL,
                    P8EST_STRING "_vtk: Error writing vtk header");

    /* do not write the tree id's of each quadrant
     * (there is only one tree in this example) */
    context = p8est_vtk_write_cell_dataf(context, 0, 1,  /* do write the refinement level of each quadrant */
                                         1,      /* do write the mpi process id of each quadrant */
                                         0,      /* do not wrap the mpi rank (if this were > 0, the modulus of the rank relative to this number would be written instead of the rank) */
                                         0,      /* there is no custom cell scalar data. */
                                         0,      /* there is no custom cell vector data. */
                                         context);       /* mark the end of the variable cell data. */
    SC_CHECK_ABORT (context != NULL,
                    P8EST_STRING "_vtk: Error writing cell data");

    /* write one scalar field: the solution value */
    context = p8est_vtk_write_point_dataf(context, 1, 0,
                                          "boundary",
                                          boundary_data,
                                          context);
    SC_CHECK_ABORT (context != NULL,
                    P8EST_STRING "_vtk: Error writing cell data");

    const int retval = p8est_vtk_write_footer(context);
    SC_CHECK_ABORT (!retval,
                    P8EST_STRING "_vtk: Error writing footer");

    sc_array_destroy(boundary_data);
}

int main (int argc, char **argv) {
    int                     i;
    char                    filename[BUFSIZ] = { '\0' };
    int                     mpiret;
    sc_MPI_Comm             mpicomm;
    context_t               ctx;
    p8est_t                 *p8est;
    p8est_connectivity_t    *conn;
    clock_t                 start;
    FILE                    *file;

    start = clock();

    ctx.center[0] = 0.5;
    ctx.center[1] = 0.5;
    ctx.center[2] = 0.5;

    ctx.width = 0.2;
    ctx.level = 1;
    ctx.dt = 0;

    ctx.Adiabatic = 1.4; //air

    /* Initialize MPI; see sc_mpi.h.
     * If configure --enable-mpi is given these are true MPI calls.
     * Else these are dummy functions that simulate a single-processor run. */
    mpiret = sc_MPI_Init (&argc, &argv);
    SC_CHECK_MPI (mpiret);
    mpicomm = sc_MPI_COMM_WORLD;

    /* These functions are optional.  If called they store the MPI rank as a
     * static variable so subsequent global p4est log messages are only issued
     * from processor zero.  Here we turn off most of the logging; see sc.h. */
    sc_init (mpicomm, 1, 1, NULL, SC_LP_PRODUCTION);
    p4est_init(NULL, SC_LP_PRODUCTION);

    conn = p8est_connectivity_new_unitcube();

    p8est = p8est_new_ext (mpicomm, /* communicator */
                           conn,    /* connectivity */
                           0,       /* minimum quadrants per MPI process */
                           2,       /* minimum level of refinement */
                           1,       /* fill uniform */
                           sizeof (data_t),         /* data size */
                           init,  /* initializes data */
                           (void *) (&ctx));              /* context */

    p8est_refine(p8est, 1, refine_fn, init);
    //p8est_coarsen(p8est, 1, coarsen_fn, init);

    p8est_balance(p8est, P4EST_CONNECT_FULL, init);
    p8est_partition (p8est, 1, NULL);

    // init time
    if(p8est->mpirank == 0) {
        file = fopen("init.time", "w");
        SC_CHECK_ABORT(file, "Can't write to file");
        fprintf(file, "Init took %f seconds\n", ((double) clock() - start) / CLOCKS_PER_SEC);
        fclose(file);
    }

    p8est_vtk_write_file (p8est, NULL, "init");

    for (i = 0; i < ctx.level; ++i)
    {
        start = clock();
        solve(p8est, i);

        // solve time for i-step
        if(p8est->mpirank == 0)
        {
            snprintf (filename, 17, "solution_%02d.time", i);
            file = fopen(filename, "w");
            SC_CHECK_ABORT(file, "Can't write to file");
            fprintf(file, "Solution took %f seconds\n", ((double)clock() - start)/CLOCKS_PER_SEC);
            fclose(file);
        }

        if (i == ctx.level - 1)
            break;

        // repartition
        p8est_refine(p8est, 0, refine_always, init);
        p8est_partition (p8est, 1, NULL);
    }

    // clear
    p8est_destroy(p8est);
    p8est_connectivity_destroy(conn);

    /* Verify that allocations internal to p4est and sc do not leak memory.
     * This should be called if sc_init () has been called earlier. */
    sc_finalize ();

    /* This is standard MPI programs.  Without --enable-mpi, this is a dummy. */
    mpiret = sc_MPI_Finalize ();
    SC_CHECK_MPI (mpiret);

    return 0;
}