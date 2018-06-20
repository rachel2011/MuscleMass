#define GLEW_STATIC
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "SymplecticIntegrator.h"

#include "Rigid.h"
#include "Joint.h"
#include "MatlabDebug.h"
#include "Spring.h"
#include "Particle.h"
#include "QuadProgMosek.h"
#include "Scene.h"
#include "Program.h"
#include "MatrixStack.h"

#include <iostream>
#include <iomanip>

# include <cstdlib>

# include <iomanip>
# include <cmath>
# include <ctime>

using namespace std;
using namespace Eigen;
#include <unsupported/Eigen/MatrixFunctions> // TODO: avoid using this later, write a func instead
double inf = numeric_limits<double>::infinity();

SymplecticIntegrator::SymplecticIntegrator(vector< shared_ptr<Rigid> > _boxes, vector< shared_ptr<Joint>> _joints, vector< shared_ptr<Spring> > _springs, bool _isReduced, int _num_samples, Vector3d _grav, double _epsilon):
		num_joints(_boxes.size() - 1),
		num_samples(_num_samples),
		boxes(_boxes),
		joints(_joints),
		springs(_springs),
		isReduced(_isReduced),
		epsilon(_epsilon),
		grav(_grav)
{

	if (isReduced) {
		m = 6 * (int)boxes.size();
		n = 6 + 1 * ((int)boxes.size() - 1);
		M.resize(m, m);	
		J.resize(m, n);
		f.resize(m);
		M.setZero();
		J.setZero();
		f.setZero();
	}
	else {
		n = 6 * (int)boxes.size() + 6 + 5 * ((int)boxes.size() - 1);
	}

	A.resize(n, n);
	x.resize(n);
	b.resize(n);

	A.setZero();
	x.setZero();
	b.setZero();
}

MatrixXd SymplecticIntegrator::getJ_twist_thetadot() {
	J.setZero();
	int j = 6;

	// Rotate about Z axis
	Vector6d z;
	z.setZero();
	z(2) = 1.0;

	for (int i = 0; i < (int)boxes.size(); i++) {
		auto box = boxes[i];

		M.block<6, 6>(6 * i, 6 * i) = box->getMassMatrix();

		if (i == 0) {
			Matrix6d I;
			I.setIdentity();
			J.block<6, 6>(0, 0) = I;
		}
		else if (i == 1) {
			auto joint = box->getJoint();
			Matrix6d Ad_C_J = Rigid::adjoint(joint->getE_C_J());
			Matrix6d Ad_J_P = Rigid::adjoint(joint->getE_P_J().inverse());
			Matrix6d Ad_C_P = Ad_C_J * Ad_J_P;
			Vector6d Adz_C_J = Ad_C_J * z;

			J.block<6, 6>(j, 0) = Ad_C_P;
			J.block<6, 1>(j, 5 + i) = Adz_C_J;
			j = j + 6;
		}
		else {
			auto joint = box->getJoint();
			Matrix6d Ad_C_J = Rigid::adjoint(joint->getE_C_J());
			Matrix6d Ad_J_P = Rigid::adjoint(joint->getE_P_J().inverse());
			Vector6d Adz_C_J = Ad_C_J * z;
			Matrix6d Ad_C_P = Ad_C_J * Ad_J_P;

			J.block(j, 0, 6, 5 + i) = Ad_C_P * J.block(j - 6, 0, 6, 5 + i);
			J.block<6, 1>(j, 5 + i) = Adz_C_J;
			j = j + 6;
		}
	}
	return J;
}

void SymplecticIntegrator::step(double h) {
	A.setZero();
	x.setZero();
	b.setZero();

	// Solve linear system
	if (isReduced) {
		M.setZero();
		J.setZero();
		f.setZero();

		VectorXd thetadotlist = Joint::getThetadotVector(joints);

		J = getJ_twist_thetadot();

		for (int i = 0; i < (int)boxes.size(); i++) {
			auto box = boxes[i];
			f.segment<6>(6 * i) = box->getMassMatrix() * box->getTwist() + h * box->getForce();
			//f.segment<6>(6 * i) = h * box->getForce();
    		}

		A = J.transpose() * M * J;
		b = J.transpose() * f;
		
		// Compute the inertia matrix of spring using finite difference 
		MatrixXd M_s = Spring::computeMassMatrix(springs, (int)boxes.size(), isReduced);

		x.setZero();
		MatrixXd JJ = J.block(0, n - num_joints, m, num_joints);
		
		A = JJ.transpose() * M * JJ;
		A += M_s;
		
		VectorXd b_s = Spring::computeGravity(springs, (int)boxes.size(), isReduced);
	
		x.segment(6, num_joints) = A.ldlt().solve(b.segment(6, num_joints) + b_s * h + M_s * thetadotlist);	// thetadot

		cout << x.segment(6, num_joints) << endl << endl;
		VectorXd phi = JJ * x.segment(6, num_joints);

		// For QP
		VectorXd xl, xu; // lower, upper bound 
		xl.resize(num_joints); 
		xu.resize(num_joints);
		bool isQP = false;

		for (int i = 1; i < (int)boxes.size(); i++) {
			auto box = boxes[i];
			
			// Check if the new joint angle is out of range
			double dtheta = h * x(5 + i);
			double theta_old = box->getJoint()->getTheta();
			double theta_new = theta_old + dtheta;

			double min_theta = box->getJoint()->getMinTheta();
			double max_theta = box->getJoint()->getMaxTheta();
			
			// Two cases need to solve QP 
			if (theta_new > max_theta) {
				// thetadot <= 0.0 
				xl(i - 1) = -inf;
				xu(i - 1) = 0.0;
				isQP = true;
			}
			else if (theta_new < min_theta) {
				// thetadot >= 0.0
				xl(i - 1) = 0.0;
				xu(i - 1) = inf;
				isQP = true;
			}
			else {
				xl(i - 1) = -inf;
				xu(i - 1) = inf;
			}
		}
		if (isQP) {
			shared_ptr<QuadProgMosek> program_ = make_shared <QuadProgMosek>();
			program_->setParamInt(MSK_IPAR_OPTIMIZER, MSK_OPTIMIZER_INTPNT);
			program_->setParamInt(MSK_IPAR_LOG, 10);
			program_->setParamInt(MSK_IPAR_LOG_FILE, 1);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_DFEAS, 1e-8);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_INFEAS, 1e-10);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_MU_RED, 1e-8);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_NEAR_REL, 1e3);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_PFEAS, 1e-8);
			program_->setParamDouble(MSK_DPAR_INTPNT_QO_TOL_REL_GAP, 1e-8);

			program_->setNumberOfVariables(num_joints);
			program_->setLowerVariableBound(xl);
			program_->setUpperVariableBound(xu);

			program_->setObjectiveMatrix(A.sparseView());
			program_->setObjectiveVector(b.segment(6, num_joints));

			bool success = program_->solve();
			VectorXd sol = program_->getPrimalSolution();
			VectorXd phi = JJ * sol;

			for (int i = 1; i < (int)boxes.size(); i++) {
				auto box = boxes[i];
				// Update joint angles
				box->setRotationAngle(h * sol(i - 1));
				box->setThetadot(sol(i - 1));
				// Don't forget to update twists as well, we will use it to compute forces
				box->setTwist(phi.segment<6>(6 * i));			
			}
		}
		else{

			// Use the result of KKT
			for (int i = 0; i < (int)boxes.size(); i++) {
				auto box = boxes[i];
				if (i != 0) {
					// Update joint angles
					box->setRotationAngle(h * x(5 + i));
					box->setThetadot(x(5 + i));
					// Don't forget to update twists as well, we will use it to compute forces
					box->setTwist(phi.segment<6>(6 * i));
				}
			}
		}	
	}
	else {
		VectorXd boxtwists;
		boxtwists.resize((int)6 * boxes.size());
		boxtwists.setZero();

		// Maximal Coordinate
		int j = 0;
		for (int i = 0; i < (int)boxes.size(); i++) {
			auto box = boxes[i];

			A.block<6, 6>(6 * i, 6 * i) = box->getMassMatrix();
			boxtwists.segment<6>(6 * i) = box->getTwist();
			b.segment<6>(6 * i) = h * box->getForce();
			if (i == 0) {
				Matrix6d I;
				I.setIdentity();
				A.block<6, 6>(6 * (int)boxes.size(), 0) = I;
				A.block<6, 6>(0, 6 * (int)boxes.size()) = I;
			}
			else {
				auto joint = box->getJoint();
				Matrix6d Ad_J_P = Rigid::adjoint(joint->getE_P_J().inverse());
				Matrix6d Ad_J_C = -Rigid::adjoint(joint->getE_C_J().inverse());
				int id_P = box->getParent()->getIndex();
				int id_C = box->getIndex();

				Matrix5x6d G_J_P;
				G_J_P.block<2, 6>(0, 0) = Ad_J_P.block<2, 6>(0, 0);
				G_J_P.block<3, 6>(2, 0) = Ad_J_P.block<3, 6>(3, 0);

				Matrix5x6d G_J_C;
				G_J_C.block<2, 6>(0, 0) = Ad_J_C.block<2, 6>(0, 0);
				G_J_C.block<3, 6>(2, 0) = Ad_J_C.block<3, 6>(3, 0);

				A.block<5, 6>(6 * (int)boxes.size() + 6 + 5 * j, 6 * id_P) = G_J_P;
				A.block<6, 5>(6 * id_P, 6 * (int)boxes.size() + 6 + 5 * j) = G_J_P.transpose();

				A.block<5, 6>(6 * (int)boxes.size() + 6 + 5 * j, 6 * id_C) = G_J_C;
				A.block<6, 5>(6 * id_C, 6 * (int)boxes.size() + 6 + 5 * j) = G_J_C.transpose();

				j++;
			}
		}

		MatrixXd M_s = Spring::computeMassMatrix(springs, (int)boxes.size(), isReduced);
		// Add mass matrix of spring to A matrix
		A.block(0, 0, 6 * (int)boxes.size(), 6 * (int)boxes.size()) += M_s;

		VectorXd b_s = Spring::computeGravity(springs, (int)boxes.size(), isReduced);
		b.segment(0, 6 * (int)boxes.size()) += A.block(0, 0, 6 * (int)boxes.size(), 6 * (int)boxes.size()) * boxtwists + b_s * h;

		x = A.ldlt().solve(b);

		// Update boxes
		for (int i = 0; i < (int)boxes.size(); i++) {
			auto box = boxes[i];
			box->setTwist(x.segment<6>(6 * i));
		}
	}
}

void SymplecticIntegrator::draw(shared_ptr<MatrixStack> MV, const shared_ptr<Program> prog, shared_ptr<MatrixStack> P) const
{	
	prog->bind();
	glUniformMatrix4fv(prog->getUniform("P"), 1, GL_FALSE, glm::value_ptr(P->topMatrix()));
	glUniformMatrix4fv(prog->getUniform("MV"), 1, GL_FALSE, glm::value_ptr(MV->topMatrix()));
	MV->pushMatrix();
	glColor3f(1.0, 0.0, 0.0); // black
	glPointSize(3.0);
	glBegin(GL_POINTS);
	for (int i = 0; i < (int)debug_points.size(); ++i) {
		Vector3f p = debug_points[i]->x.cast<float>();
		glVertex3f(p(0), p(1), p(2));
	}	
	glEnd();
	MV->popMatrix();
	prog->unbind();
}

SymplecticIntegrator::~SymplecticIntegrator() {

}