##########################################################################
#    This file is part of ssm.
#
#    ssm is free software: you can redistribute it and/or modify it
#    under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    ssm is distributed in the hope that it will be useful, but
#    WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#    General Public License for more details.
#
#    You should have received a copy of the GNU General Public
#    License along with ssm.  If not, see
#    <http://www.gnu.org/licenses/>.
#########################################################################

import os
import os.path
import tarfile
import shutil
import json

from Ccoder import Ccoder
from Data import Data

from jinja2 import Environment, FileSystemLoader

class Builder(Data, Ccoder):
    """build a model"""

    def __init__(self, path_rendered, dpkgRoot, dpkg,  **kwargs):
        Ccoder.__init__(self, dpkgRoot, dpkg, **kwargs)
        Data.__init__(self, path_rendered, dpkgRoot, dpkg,  **kwargs)

        self.path_rendered = os.path.abspath(path_rendered)
        self.env = Environment(loader=FileSystemLoader(os.path.join(self.path_rendered, 'C', 'templates')))
        self.env.filters.update({
            'is_prior': lambda x: ('data' in x) and isinstance(x['data'], dict) and ('data' in x['data']) and ('distribution' in x['data']['data'])
        })

    def prepare(self, path_templates=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'C', 'templates'), replace=True):
        """
        copy templates to path_rendered
        """

        ##this function is called only when a new user has created or edited a model whose name is unique (primary key) so it is the only one able to recreate a model...
        if replace:
            if os.path.exists(self.path_rendered):
                shutil.rmtree(self.path_rendered)

        #copy templates to uploads/rendered/user_name/model_id
        if not os.path.exists(self.path_rendered):
            shutil.copytree(path_templates, os.path.join(self.path_rendered, 'C', 'templates'))

    def archive(self, replace=True):
        """make a tarball"""

        tar = tarfile.open(os.path.join(os.path.dirname(self.path_rendered), os.path.basename(self.path_rendered)+'.tar.gz'), "w:gz")
        tar.add(self.path_rendered, arcname=os.path.basename(self.path_rendered))
        tar.close()

        if replace:
            if os.path.exists(self.path_rendered):
                shutil.rmtree(self.path_rendered)

    def render(self, prefix, data):

        template = self.env.get_template(prefix + '_template.c')
        with open(os.path.join(self.path_rendered, 'C', 'templates', prefix + ".c"), "w") as f:
            f.write(template.render(data))
            os.remove(os.path.join(self.path_rendered, 'C', 'templates', prefix + '_template.c'))

    def code(self):
        """generate C code for MIF, Simplex, pMCMC, Kalman, simulation, ..."""

        is_diff = True if len(self.par_diff) > 0 else False

        orders = self.orders()

        ##methods whose results are use multiple times
        step_ode_sde = self.step_ode_sde()
        jac = self.jac(step_ode_sde['sf'])

        self.render('ode_sde', {'is_diff': is_diff, 'step':step_ode_sde, 'orders': orders})

        parameters = self.parameters()
        parameters['orders'] = orders
        self.render('transform', parameters)
        self.render('input', parameters)

        observed = self.observed()
        observed['orders'] = orders
        observed['h_grads'] = self.h_grads()
        self.render('observed', observed)

        self.render('iterator', {'iterators':self.iterators()})

        psr = {
            'orders': orders,
            'alloc': self.alloc_psr(),
            'is_diff': is_diff,
            'white_noise': self.white_noise,
            'step': self.step_psr(),
            'step_inc': self.step_psr_inc(),
            'psr_multinomial': self.step_psr_multinomial()
        }
        self.render('psr', psr)

        self.render('diff', {'diff': self.compute_diff(), 'orders': orders})

        self.render('Q', {'Q': self.eval_Q(), 'is_diff': is_diff, 'orders': orders})

        self.render('Ht', {'Ht': self.Ht(), 'is_diff': is_diff, 'orders': orders})

        self.render('jac', {'jac': jac, 'is_diff': is_diff, 'orders': orders})

        self.render('step_ekf', {'is_diff': is_diff, 'step': step_ode_sde, 'orders': orders})

        self.render('check_ic', parameters)

    def write_data(self):

        reset_all = []
        for x in self.obs_model:
            reset_all += [self.order_states[s] for s in self.get_inc_reset(x)]

        x = {'start': self.t0.isoformat(), 'data': self.prepare_data(), 'covariates': self.prepare_covariates(), 'reset_all': list(set(reset_all))}
        with open(os.path.join(self.path_rendered, ".data.json"), "w") as f:
            json.dump(x, f)



if __name__=="__main__":


    dpkgRoot = os.path.join('..' ,'examples', 'sir')
    dpkg = json.load(open(os.path.join(dpkgRoot, 'package.json')))
    b = Builder(os.path.join(dpkgRoot, 'bin'), dpkgRoot, dpkg)

    b.prepare()
    b.code()
    b.write_data()
