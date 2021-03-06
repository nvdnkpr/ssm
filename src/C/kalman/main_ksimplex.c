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

struct s_ksimplex
{
    ssm_data_t *data;
    ssm_nav_t *nav;
    ssm_calc_t *calc;
    ssm_input_t *input;
    ssm_par_t *par;
    ssm_X_t *X;
    ssm_fitness_t *fitness;

    int flag_prior;
};

/**
 * function to **minimize**
 */
static double f_ksimplex(const gsl_vector *theta, void *params)
{
    unsigned int n, t0, t1;

    struct s_ksimplex *p = (struct s_ksimplex *) params;
    ssm_data_t *data = p->data;
    ssm_nav_t *nav = p->nav;
    ssm_calc_t *calc = p->calc;
    ssm_input_t *input = p->input;
    ssm_par_t *par = p->par;
    ssm_X_t *X = p->X;
    ssm_fitness_t *fitness = p->fitness;
    int flag_prior = p->flag_prior;

    if(ssm_check_ic(par, calc) != SSM_SUCCESS){
        if (nav->print & SSM_PRINT_WARNING) {
            ssm_print_warning("constraints on initial conditions have not been respected: assigning worst possible fitness");
        }
        return GSL_POSINF; //GSL simplex algo minimizes so we return POSINF
    }


    ssm_theta2input(input, (gsl_vector *) theta, nav);
    ssm_input2par(par, input, calc, nav);
    ssm_par2X(X, par, calc, nav);
    X->dt = X->dt0;
    fitness->log_like = 0.0;
    fitness->cum_status[0] = SSM_SUCCESS;

    ssm_kalman_reset_Ct(X, nav);

    for(n=0; n<data->n_obs; n++) {
        t0 = (n) ? data->rows[n-1]->time: 0;
        t1 = data->rows[n]->time;

        ssm_X_reset_inc(X, data->rows[n], nav);

        fitness->cum_status[0] |= ssm_f_prediction_ode(X, t0, t1, par, nav, calc);
	
        if(data->rows[n]->ts_nonan_length && (fitness->cum_status[0] == SSM_SUCCESS)) {
            fitness->cum_status[0] |= ssm_kalman_update(fitness, X, data->rows[n], t1, par, calc, nav);
        }

	if(fitness->cum_status[0] != SSM_SUCCESS){
	    if (nav->print & SSM_PRINT_WARNING) {
		ssm_print_warning("warning: something went wrong");
	    }
	    return GSL_POSINF; //GSL simplex algo minimizes so we return POSINF
	}
    }

    if (flag_prior) {
        double log_prob_prior_value;
        ssm_err_code_t rc = ssm_log_prob_prior(&log_prob_prior_value, (ssm_theta_t *) theta, nav, fitness);
        if(rc != SSM_SUCCESS){
            if(nav->print & SSM_PRINT_WARNING){
                ssm_print_warning("error log_prob_prior computation");
            }
            return GSL_POSINF; //GSL simplex algo minimizes so we multiply by -1
        } else {
            fitness->log_like += log_prob_prior_value;
        }
    }

    return -fitness->log_like;  //GSL simplex algo minimizes so we multiply by -1 (log likelihood)
}


int main(int argc, char *argv[])
{
    ssm_options_t *opts = ssm_options_new();
    ssm_options_load(opts, SSM_KSIMPLEX, argc, argv);

    json_t *jparameters = ssm_load_json_stream(stdin);
    json_t *jdata = ssm_load_data(opts);

    ssm_nav_t *nav = ssm_nav_new(jparameters, opts);
    ssm_data_t *data = ssm_data_new(jdata, nav, opts);
    ssm_fitness_t *fitness = ssm_fitness_new(data, opts);
    ssm_calc_t *calc = ssm_calc_new(jdata, nav, data, fitness, opts, 0);
    ssm_X_t *X = ssm_X_new(nav, opts);

    json_decref(jdata);

    ssm_input_t *input = ssm_input_new(jparameters, nav);
    ssm_par_t *par = ssm_par_new(input, calc, nav);
    ssm_theta_t *theta = ssm_theta_new(input, nav);
    ssm_var_t *var = ssm_var_new(jparameters, nav);

    struct s_ksimplex params = {data, nav, calc, input, par, X, fitness, opts->flag_prior};

    double maximized_fitness = ssm_simplex(theta, var, &params, &f_ksimplex, nav, opts);

    if (!(nav->print & SSM_PRINT_LOG)) {	
	if(opts->flag_prior){
	    fitness->summary_log_ltp = maximized_fitness;
	} else {
	    fitness->log_like = maximized_fitness;
	    ssm_aic(fitness, nav, fitness->log_like);
	}
	ssm_pipe_theta(stdout, jparameters, theta, NULL, fitness, nav, opts);
    }

    json_decref(jparameters);

    ssm_X_free(X);
    ssm_calc_free(calc, nav);
    ssm_data_free(data);
    ssm_nav_free(nav);
    ssm_fitness_free(fitness);

    ssm_input_free(input);
    ssm_par_free(par);
    ssm_theta_free(theta);
    ssm_var_free(var);

    return 0;
}
