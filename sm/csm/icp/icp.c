#include <math.h>
#include <string.h>

#include <gsl/gsl_matrix.h>

#include <gpc/gpc.h>
#include <egsl/egsl_macros.h>

#include "../csm_all.h"

#include "icp.h"

#include "../csm_all.h"

void sm_journal_open(const char* file) {
	file = 0;
/*	journal_open(file);*/
}

void sm_icp(struct sm_params*params, struct sm_result*res) {
	res->valid = 0;
	
	if(!ld_valid_fields(params->laser_ref) || 
	   !ld_valid_fields(params->laser_sens)) {
		return;
	}
	
	if(JJ) jj_context_enter("sm_icp");
	
	egsl_push();
	
	LDP laser_ref  = params->laser_ref;
	LDP laser_sens = params->laser_sens;
			
	if(params->use_corr_tricks || params->debug_verify_tricks)
		ld_create_jump_tables(laser_ref);
		
	ld_compute_cartesian(laser_ref);
	ld_compute_cartesian(laser_sens);

	if(params->do_alpha_test) {
		ld_simple_clustering(laser_ref, params->clustering_threshold);
		ld_compute_orientation(laser_ref, params->orientation_neighbourhood, params->sigma);
		ld_simple_clustering(laser_sens, params->clustering_threshold);
		ld_compute_orientation(laser_sens, params->orientation_neighbourhood, params->sigma);
	}

	if(JJ) jj_add("laser_ref",  ld_to_json(laser_ref));
	if(JJ) jj_add("laser_sens", ld_to_json(laser_sens));
	
	gsl_vector * x_new = gsl_vector_alloc(3);
	gsl_vector * x_old = vector_from_array(3, params->first_guess);
	
	if(params->do_visibility_test) {
		sm_debug("laser_ref:\n");
		visibilityTest(laser_ref, x_old);

		sm_debug("laser_sens:\n");
		gsl_vector * minus_x_old = gsl_vector_alloc(3);
		ominus(x_old,minus_x_old);
		visibilityTest(laser_sens, minus_x_old);
		gsl_vector_free(minus_x_old);
	}
	
	double error;
	int iterations;
	int nvalid;
	if(!icp_loop(params, x_old->data, x_new->data, &error, &nvalid, &iterations)) {
		sm_error("ICP not complete. \n");
		return;
	}

	double best_error = error;
	gsl_vector * best_x = gsl_vector_alloc(3);
	gsl_vector_memcpy(best_x, x_new);

	if(params->restart && 
		(error/nvalid)>(params->restart_threshold_mean_error) ) {
		sm_debug("Restarting: %f > %f \n",(error/nvalid),(params->restart_threshold_mean_error));
		double dt  = params->restart_dt;
		double dth = params->restart_dtheta;
		sm_debug("icp_loop: dt = %f dtheta= %f deg\n",dt,rad2deg(dth));
		
		double perturb[6][3] = {
			{dt,0,0}, {-dt,0,0},
			{0,dt,0}, {0,-dt,0},
			{0,0,dth}, {0,0,-dth}
		};

		int a; for(a=0;a<6;a++){
			sm_debug("-- Restarting with perturbation #%d\n", a);
			struct sm_params my_params = *params;
			gsl_vector * start = gsl_vector_alloc(3);
				gvs(start, 0, gvg(x_new,0)+perturb[a][0]);
				gvs(start, 1, gvg(x_new,1)+perturb[a][1]);
				gvs(start, 2, gvg(x_new,2)+perturb[a][2]);
			gsl_vector * x_a = gsl_vector_alloc(3);
			double my_error; int my_valid; int my_iterations;
			if(!icp_loop(&my_params, start->data, x_a->data, &my_error, &my_valid, &my_iterations)){
				sm_error("Error during restart #%d/%d. \n", a, 6);
				break;
			}
			iterations+=my_iterations;
		
			if(my_error < best_error) {
				sm_debug("--Perturbation #%d resulted in error %f < %f\n", a,my_error,best_error);
				gsl_vector_memcpy(best_x, x_a);
				best_error = my_error;
			}
			gsl_vector_free(x_a); gsl_vector_free(start);
		}
	}
	
	
	/* At last, we did it. */
	res->valid = 1;
	vector_to_array(best_x, res->x);
	sm_debug("icp: final x =  %s  \n", gsl_friendly_pose(best_x));
	
	
	if(params->do_compute_covariance)  {

		val cov0_x, dx_dy1, dx_dy2;
		compute_covariance_exact(
			laser_ref, laser_sens, best_x,
			&cov0_x, &dx_dy1, &dx_dy2);
		
		val cov_x = sc(square(params->sigma), cov0_x); 
/*		egsl_v2da(cov_x, res->cov_x); */
		
		res->cov_x_m = egsl_v2gslm(cov_x);
		res->dx_dy1_m = egsl_v2gslm(dx_dy1);
		res->dx_dy2_m = egsl_v2gslm(dx_dy2);
		
		if(0) {
			egsl_print("cov0_x", cov0_x);
			egsl_print_spectrum("cov0_x", cov0_x);
		
			val fim = ld_fisher0(laser_ref);
			val ifim = inv(fim);
			egsl_print("fim", fim);
			egsl_print_spectrum("ifim", ifim);
		}
	}
	
	
	res->error = best_error;
	res->iterations = iterations;
	res->nvalid = nvalid;

	gsl_vector_free(x_new);
	gsl_vector_free(x_old);
	gsl_vector_free(best_x);
	egsl_pop();

	if(JJ) jj_context_exit();
}
