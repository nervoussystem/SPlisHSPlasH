#include "Viscosity_Peer2015.h"
#include "SPlisHSPlasH/TimeManager.h"
#include "Utilities/Timing.h"

using namespace SPH;
using namespace GenParam;

int Viscosity_Peer2015::ITERATIONS = -1;
int Viscosity_Peer2015::MAX_ITERATIONS = -1;
int Viscosity_Peer2015::MAX_ERROR = -1;


Viscosity_Peer2015::Viscosity_Peer2015(FluidModel *model) :
	ViscosityBase(model)
{
	m_targetNablaV.resize(model->numParticles(), Matrix3r::Zero());

	m_iterations = 0;
	m_maxIter = 50;
	m_maxError = 0.01;
}

Viscosity_Peer2015::~Viscosity_Peer2015(void)
{
	m_targetNablaV.clear();
}

void Viscosity_Peer2015::initParameters()
{
	ViscosityBase::initParameters();

	ITERATIONS = createNumericParameter("viscoIterations", "Iterations", &m_iterations);
	setGroup(ITERATIONS, "Viscosity");
	setDescription(ITERATIONS, "Iterations required by the viscosity solver.");
	getParameter(ITERATIONS)->setReadOnly(true);

	MAX_ITERATIONS = createNumericParameter("viscoMaxIter", "Max. iterations (visco)", &m_maxIter);
	setGroup(MAX_ITERATIONS, "Viscosity");
	setDescription(MAX_ITERATIONS, "Coefficient for the viscosity force computation");
	static_cast<NumericParameter<unsigned int>*>(getParameter(MAX_ITERATIONS))->setMinValue(1);

	MAX_ERROR = createNumericParameter("viscoMaxError", "Max. visco error", &m_maxError);
	setGroup(MAX_ERROR, "Viscosity");
	setDescription(MAX_ERROR, "Coefficient for the viscosity force computation");
	RealParameter* rparam = static_cast<RealParameter*>(getParameter(MAX_ERROR));
	rparam->setMinValue(1e-6);
}

void Viscosity_Peer2015::matrixVecProd(const Real* vec, Real *result, void *userData)
{
	FluidModel *model = (FluidModel*)userData;
	const unsigned int numParticles = model->numActiveParticles();

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) 
		for (int i = 0; i < (int)numParticles; i++)
		{
			// Diagonal element
			const Vector3r &xi = model->getPosition(0, i);
			result[i] = (model->getDensity(i) - model->getMass(i) * model->W_zero()) * vec[i];

			for (unsigned int j = 0; j < model->numberOfNeighbors(0, i); j++)
			{
				const unsigned int neighborIndex = model->getNeighbor(0, i, j);
				const Vector3r &xj = model->getPosition(0, neighborIndex);
				result[i] -= model->getMass(neighborIndex) * model->W(xi - xj) * vec[neighborIndex];
			}
		}
	}
}

void Viscosity_Peer2015::diagonalMatrixElement(const unsigned int row, Real &result, void *userData)
{
	// Diagonal element
	FluidModel *model = (FluidModel*)userData;
	result = model->getDensity(row) - model->getMass(row) * model->W_zero();
}

void Viscosity_Peer2015::step()
{
	const int numParticles = (int) m_model->numActiveParticles();
	const Real viscosity = 1.0 - m_viscosity;
	const Real density0 = m_model->getValue<Real>(FluidModel::DENSITY0);

	const Real h = TimeManager::getCurrent()->getTimeStepSize();

	// Compute target
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) nowait 
		for (int i = 0; i < numParticles; i++)
		{
			const Vector3r &xi = m_model->getPosition(0, i);
			const Vector3r &vi = m_model->getVelocity(0, i);
			const Real density_i = m_model->getDensity(i);

			Matrix3r nablaV;
			nablaV.setZero();
			for (unsigned int j = 0; j < m_model->numberOfNeighbors(0, i); j++)
			{
				const unsigned int neighborIndex = m_model->getNeighbor(0, i, j);

				const Vector3r &xj = m_model->getPosition(0, neighborIndex);
				const Vector3r &vj = m_model->getVelocity(0, neighborIndex);
				const Vector3r gradW = m_model->gradW(xi - xj);

				Matrix3r dyad = (vj - vi) * gradW.transpose();

				nablaV += (1.0 / density_i) * m_model->getMass(neighborIndex) * dyad;
			}

			Matrix3r &target = getTargetNablaV(i);
			const Matrix3r R = 0.5 * (nablaV - nablaV.transpose());
			const Real divergence = nablaV(0, 0) + nablaV(1, 1) + nablaV(2, 2);
			const Matrix3r V = (1.0 / 3.0) * divergence * Matrix3r::Identity();
			const Matrix3r S = 0.5 * (nablaV + nablaV.transpose()) - V;
			if (density_i >= density0)
			{
				target = R + V + viscosity * S;
			}
			else
			{
				if (-divergence < 0.0)
					target = R + V + viscosity * S;
				else
					target = R + viscosity * S;
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// Init linear system solver and preconditioner
	//////////////////////////////////////////////////////////////////////////
	MatrixReplacement A(m_model->numActiveParticles(), matrixVecProd, (void*) m_model);
	m_solver.preconditioner().init(m_model->numActiveParticles(), diagonalMatrixElement, (void*)m_model);

	m_solver.setTolerance(m_maxError);
	m_solver.setMaxIterations(m_maxIter);
	m_solver.compute(A);
	m_iterations = static_cast<unsigned int>(m_solver.iterations());

	Eigen::VectorXd b0(numParticles);
	Eigen::VectorXd b1(numParticles);
	Eigen::VectorXd b2(numParticles);
	Eigen::VectorXd x0(numParticles);
	Eigen::VectorXd x1(numParticles);
	Eigen::VectorXd x2(numParticles);
	Eigen::VectorXd g0(numParticles);
	Eigen::VectorXd g1(numParticles);
	Eigen::VectorXd g2(numParticles);

	//////////////////////////////////////////////////////////////////////////
	// Compute RHS
	//////////////////////////////////////////////////////////////////////////
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) nowait 
		for (int i = 0; i < (int)numParticles; i++)
		{
			const Vector3r &xi = m_model->getPosition(0, i);
			Vector3r rhs;
			rhs.setZero();

			for (unsigned int j = 0; j < m_model->numberOfNeighbors(0, i); j++)
			{
				const unsigned int neighborIndex = m_model->getNeighbor(0, i, j);

				const Real m = m_model->getMass(neighborIndex);
				const Vector3r &xj = m_model->getPosition(0, neighborIndex);
				const Vector3r xij = xi - xj;
				const Real W = m_model->W(xij);

				rhs += m * 0.5 * (getTargetNablaV(i) + getTargetNablaV(neighborIndex)) * xij * W;
			}

			const Vector3r &vi = m_model->getVelocity(0, i);
			g0[i] = vi[0];
			g1[i] = vi[1];
			g2[i] = vi[2];
			b0[i] = rhs[0];
			b1[i] = rhs[1];
			b2[i] = rhs[2];
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// Solve linear system 
	//////////////////////////////////////////////////////////////////////////
	START_TIMING("CG solve");
	int iter = 0;
	x0 = m_solver.solveWithGuess(b0, g0);
	if (m_solver.iterations() == 0)
		x0 = g0;
	iter += (int)m_solver.iterations();
	x1 = m_solver.solveWithGuess(b1, g1);
	if (m_solver.iterations() == 0)
		x1 = g1;
	iter += (int)m_solver.iterations();
	x2 = m_solver.solveWithGuess(b2, g2);
	if (m_solver.iterations() == 0)
		x2 = g2;
	iter += (int)m_solver.iterations();
	STOP_TIMING_AVG;

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) nowait 
		for (int i = 0; i < (int)numParticles; i++)
		{
			Vector3r &vi = m_model->getVelocity(0, i);
			vi[0] = x0[i];
			vi[1] = x1[i];
			vi[2] = x2[i];
		}
	}
}


void Viscosity_Peer2015::reset()
{
}

void Viscosity_Peer2015::performNeighborhoodSearchSort()
{
}
