/**************************************************************************
 *    This file is part of ssm.
 *
 *    ssm is free software: you can redistribute it and/or modify it
 *    under the terms of the GNU General Public License as published
 *    by the Free Software Foundation, either version 3 of the
 *    License, or (at your option) any later version.
 *
 *    ssm is distributed in the hope that it will be useful, but
 *    WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public
 *    License along with ssm.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "ssm.h"


/**
 * Brings Ct back to being symetric and semi-definite positive,
 * in case numerical instabilities made it lose these properties.
 *
 * In theory the EKF shouldn't need this,
 * and there is no right way to bring back a wrong covariance matrix to being right,
 * but this is the most natural way to go as far as I know.
 * We shouldn't need this anymore with the Square-Root Unscented Kalman Filter.
 */
ssm_err_code_t _ssm_check_and_correct_Ct(ssm_X_t *X, ssm_calc_t *calc, ssm_nav_t *nav)
{

    int i,j;
    ssm_err_code_t cum_status = SSM_SUCCESS;
    int status;

    //////////////////////
    // STEP 1: SYMMETRY //
    //////////////////////
    // Ct = (Ct + Ct')/2


    gsl_matrix *Temp = calc->_Ft; // temporary matrix
    int m = nav->states_sv_inc->length + nav->states_diff->length;
    gsl_matrix_view Ct =  gsl_matrix_view_array(&X->proj[m], m, m);
    gsl_matrix_memcpy(Temp, &Ct.matrix);	// temp = Ct

    for(i=0; i< m; i++){
        for(j=0; j< m; j++){
            gsl_matrix_set(&Ct.matrix, i, j, ((gsl_matrix_get(Temp, i, j) + gsl_matrix_get(Temp, j, i)) / 2.0) );
        }
    }

  

    ////////////////////////
    // STEP 2: POSITIVITY //
    ////////////////////////
    // Bringing negative eigen values of Ct back to zero

    // compute the eigen values and vectors of Ct
    gsl_vector *eval = calc->_eval;      // to store the eigen values
    gsl_matrix *evec = calc->_evec;      // to store the eigen vectors
    gsl_eigen_symmv_workspace *w = calc->_w_eigen_vv;

    //IMPORTANT: The diagonal and lower triangular part of Ct are destroyed during the computation so we do the computation on temp
    status = gsl_eigen_symmv(Temp, eval, evec, w);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    gsl_matrix_set_zero(Temp);

    int change_basis = 0;
    // eval = max(eval,0) and diag(eval)
    for (i=0; i<m; i++) {
        if (gsl_vector_get(eval,i) < 0.0) {
            gsl_vector_set(eval, i, 0.0); // keeps bringing negative eigen values to 0
            change_basis = 1;
        }
        gsl_matrix_set(Temp, i, i, gsl_vector_get(eval, i)); // puts the eigen values in the diagonal of temp
    }

    if(change_basis){
        gsl_matrix *Temp2 = calc->_FtCt; // temporary matrix

        //////////////////
        // basis change //
        //////////////////

        // Ct = evec * temp * evec'

        // Temp2 = 1.0*Temp*t(evec) + 0.0*Temp2
        status = gsl_blas_dgemm(CblasNoTrans, CblasTrans, 1.0, Temp, evec, 0.0, Temp2);
        cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

        // Ct = 1.0*evec*Temp2 + 0.0*Temp2;
        status = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, evec, Temp2, 0.0, &Ct.matrix);
        cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;
    }

    return cum_status;
}


/**
 * Computation of the EKF gain kt for observation data_t_ts and obs
 * jacobian ht, given estimate xk_t_ts and current covariance Ct
 */
ssm_err_code_t ssm_kalman_gain_computation(ssm_row_t *row, double t, ssm_X_t *X, ssm_par_t *par, ssm_calc_t *calc, ssm_nav_t *nav)
{

    int i, j, status;
    double tmp;
    ssm_err_code_t cum_status = SSM_SUCCESS;
    int m = nav->states_sv->length + nav->states_inc->length + nav->states_diff->length;


    // sub-matrices and sub-vectors of working variables are extracted as not all tseries are observed
    gsl_vector_view pred_error = gsl_vector_subvector(calc->_pred_error,0,row->ts_nonan_length);
    gsl_matrix_view St = gsl_matrix_submatrix(calc->_St,0,0,row->ts_nonan_length,row->ts_nonan_length);
    gsl_matrix_view Stm1 = gsl_matrix_submatrix(calc->_Stm1,0,0,row->ts_nonan_length,row->ts_nonan_length);
    gsl_matrix_view Rt = gsl_matrix_submatrix(calc->_Rt,0,0,row->ts_nonan_length,row->ts_nonan_length);
    gsl_matrix_view Tmp = gsl_matrix_submatrix(calc->_Tmp_N_SV_N_TS,0,0,m,row->ts_nonan_length);
    gsl_matrix_view Ht = gsl_matrix_submatrix(calc->_Ht,0,0,m,row->ts_nonan_length);
    gsl_matrix_view Kt = gsl_matrix_submatrix(calc->_Kt,0,0,m,row->ts_nonan_length);
    gsl_matrix_view Ct =  gsl_matrix_view_array(&X->proj[m], m, m);
    gsl_permutation *p  = gsl_permutation_alloc(row->ts_nonan_length);

    // fill Ht and Rt
    ssm_eval_Ht(X, row, t, par, nav, calc);
    for(i=0; i< row->ts_nonan_length; i++){
        for(j=0; j< row->ts_nonan_length; j++){
            if (i==j){
		tmp = row->observed[i]->f_obs_var(X, par, calc, t);
		if (tmp<SSM_ZERO_LOG){
		     gsl_matrix_set(&Rt.matrix,i,j,SSM_ZERO_LOG);
		     if(nav->print & SSM_PRINT_WARNING){
			 ssm_print_warning("Observation variance too low: fixed to SSM_ZERO_LOG.");
		     }
		} else {
		    gsl_matrix_set(&Rt.matrix,i,j,tmp);
		}
            } else {
                gsl_matrix_set(&Rt.matrix,i,j,0);
            }
        }
    }

    // pred_error = double data_t_ts - xk_t_ts
    for(i=0; i< row->ts_nonan_length; i++){
        gsl_vector_set(&pred_error.vector,i,row->values[i] - row->observed[i]->f_obs_mean(X, par, calc, t));
    }

    // positivity and symetry could have been lost when propagating Ct
    cum_status |= _ssm_check_and_correct_Ct(X, calc, nav);

    // sc_st = Ht' * Ct * Ht + sc_rt
    /*
     * here ht is a column vector to fit gsl standards,
     * rather than a row vector as it should be in theory,
     * which explains the slightly different formula we use
     */

    // workn = Ct*Ht
    status = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, &Ct.matrix, &Ht.matrix, 0.0, &Tmp.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    // sc_st = Ht' * workn;
    status = gsl_blas_dgemm(CblasTrans, CblasNoTrans, 1.0, &Ht.matrix, &Tmp.matrix, 0.0, &St.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    // sc_st = sc_st + sc_rt ;
    status  =  gsl_matrix_add(&St.matrix,&Rt.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    for(i=0; i< row->ts_nonan_length; i++){
	if (gsl_matrix_get(&St.matrix,i,i)<SSM_ZERO_LOG){
	    gsl_matrix_set(&St.matrix,i,i,SSM_ZERO_LOG);
        }
    }

    // Kt = Ct * Ht * sc_st^-1
    status = gsl_linalg_LU_decomp(&St.matrix, p, &i); // inversion requires LU decomposition
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    status = gsl_linalg_LU_invert(&St.matrix,p,&Stm1.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    status = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, &Ht.matrix, &Stm1.matrix, 0.0, &Tmp.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    status = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, &Ct.matrix, &Tmp.matrix, 0.0, &Kt.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    return cum_status;

}




ssm_err_code_t ssm_kalman_update(ssm_fitness_t *fitness, ssm_X_t *X, ssm_row_t *row, double t, ssm_par_t *par, ssm_calc_t *calc, ssm_nav_t *nav)
{

    int status;
    int m = nav->states_sv_inc->length + nav->states_diff->length;
    gsl_vector_view pred_error = gsl_vector_subvector(calc->_pred_error,0,row->ts_nonan_length);
    gsl_vector_view zero = gsl_vector_subvector(calc->_zero,0,row->ts_nonan_length);
    gsl_matrix_view Kt = gsl_matrix_submatrix(calc->_Kt,0,0,m,row->ts_nonan_length);
    gsl_matrix_view Tmp = gsl_matrix_submatrix(calc->_Tmp_N_TS_N_SV,0,0,row->ts_nonan_length,m);
    gsl_vector_view X_sv = gsl_vector_view_array(X->proj,m);
    gsl_matrix_view Ht = gsl_matrix_submatrix(calc->_Ht,0,0,m,row->ts_nonan_length);
    gsl_matrix_view Ct =  gsl_matrix_view_array(&X->proj[m], m, m);
    gsl_matrix_view St = gsl_matrix_submatrix(calc->_St,0,0,row->ts_nonan_length,row->ts_nonan_length);

    ssm_err_code_t cum_status = ssm_kalman_gain_computation(row, t, X, par, calc, nav);

    //////////////////
    // state update //
    //////////////////
    // X_sv += Kt * pred_error
    status = gsl_blas_dgemv(CblasNoTrans,1.0,&Kt.matrix,&pred_error.vector,1.0,&X_sv.vector);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;
    

    ///////////////////////
    // covariance update //
    ///////////////////////

    // Ct = Ct - Kt * Ht' * Ct
    status = gsl_blas_dgemm(CblasTrans, CblasNoTrans, 1.0, &Ht.matrix, &Ct.matrix, 0.0, &Tmp.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    status = gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, -1.0, &Kt.matrix, &Tmp.matrix, 1.0, &Ct.matrix);
    cum_status |=  (status != GSL_SUCCESS) ? SSM_ERR_KAL : SSM_SUCCESS;

    // positivity and symmetry could have been lost when updating Ct
    cum_status |= _ssm_check_and_correct_Ct(X, calc, nav);

    // positivity of state variables and remainder could have been lost when updating X_sv
    cum_status |= ssm_check_no_neg_sv_or_remainder(X, par, nav, calc, t);

    // log_like   
    fitness->log_like += ssm_sanitize_log_likelihood(log(ssm_dmvnorm(row->ts_nonan_length, &pred_error.vector, &zero.vector, &St.matrix, 1.0)), row, fitness, nav);
    return cum_status;
}



/**
 *  For eval_jac
 *
 *  Rational basis
 *  we have an equation (for instance dI/dt) named eq and let's say that we are interested in its derivative against v (we assume that v follows a diffusion)'
 *  The template gives us d eq/d v (jac_tpl)
 *  However, as v can be transform (let's say log here) we want d eq / d log(v)
 *  The chain rule gives us:
 *  d eq/ dv = d eq / d log(v) * d log(v)/dv = jac_tpl
 *  so
 *  d eq / d log(v) = ( d eq / dv ) / ( d log(v) / dv)
 *
 *  so in term of C:
 *  d eq / d log(v) = jac_tpl / f_der(v, ..)
 *  jac_der is the C term of v, provided by the template
 */
double ssm_diff_derivative(double jac_tpl, const double X[], ssm_state_t *state)
{
    if(jac_tpl){
        return jac_tpl / state->f_der(X[state->offset]);
    }

    return 0.0;
}


void ssm_kalman_reset_Ct(ssm_X_t *X, ssm_nav_t *nav)
{
    int dim = nav->states_sv_inc->length + nav->states_diff->length;
    gsl_matrix_view Ct = gsl_matrix_view_array(&X->proj[dim], dim, dim);
    gsl_matrix_set_zero(&Ct.matrix);
}
