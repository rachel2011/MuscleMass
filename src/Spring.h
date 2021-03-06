#pragma once
#ifndef MUSCLEMASS_SRC_SPRING_H_
#define MUSCLEMASS_SRC_SPRING_H_
#include <vector>
#include <memory>

#define EIGEN_DONT_ALIGN_STATICALLY
#include <Eigen/Dense>
#include "TypeDef.h"

class Program;
class MatrixStack;
class Particle;
class Rigid;
class Joint;

class Spring
{
public:
	Spring(std::shared_ptr<Particle> p0, std::shared_ptr<Particle> p1, double _mass, int num_samples, Eigen::Vector3d _grav, double _epsilon, bool _isReduced, double _stiffness);
	virtual ~Spring();
	void step(std::vector<std::shared_ptr<Joint>> joints);
	void draw(std::shared_ptr<MatrixStack> MV, const std::shared_ptr<Program> prog, const std::shared_ptr<Program> prog2, std::shared_ptr<MatrixStack> P) const;
	void setSamples(std::vector < std::shared_ptr<Particle> > _samples) { this->samples = _samples; }
	void updateSamplesPosition();
	void updateSamplesJacobian(std::vector<std::shared_ptr<Joint>> joints);
	
	
	Vector12d getBoxTwists() const { return this->phi_box; }
	Eigen::Vector2d getBoxID() const { return this->box_id; };
	double getPotentialEnergy() const { return this->V; }
	double getKineticEnergy() const { return this->K; }
	std::vector<std::shared_ptr<Particle> > getSamples() const { return this->samples; }

	double computeLength();
	void computeEnergy();
	static Eigen::MatrixXd computeMassMatrix(std::vector<std::shared_ptr<Spring> > springs, int num_boxes, bool isReduced);
	static Eigen::VectorXd computeGravity(std::vector<std::shared_ptr<Spring> > springs, int num_boxes, bool isReduced);

	std::shared_ptr<Particle> p0;
	std::shared_ptr<Particle> p1;

	double E;	// stiffness
	double L;	// initial length
	double l;	// current length
	double mass;
	double epsilon;
	double V;
	double K;

	std::vector<std::shared_ptr<Particle> > samples;	// sample points along the spring
	Eigen::Vector3d grav;
	Eigen::Vector2d box_id;
	Vector12d phi_box;	
	Eigen::VectorXd thetadotlist;
	Eigen::Vector2d thetadot;
	bool isReduced;
	std::vector< std::shared_ptr<Particle> > debug_points;
};

#endif // MUSCLEMASS_SRC_SPRING_H_
