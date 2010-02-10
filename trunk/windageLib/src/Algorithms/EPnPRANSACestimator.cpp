/* ========================================================================
 * PROJECT: windage Library
 * ========================================================================
 * This work is based on the original windage Library developed by
 *   Woonhyuk Baek (wbaek@gist.ac.kr / windage@live.com)
 *   Woontack Woo (wwoo@gist.ac.kr)
 *   U-VR Lab, GIST of Gwangju in Korea.
 *   http://windage.googlecode.com/
 *   http://uvr.gist.ac.kr/
 *
 * Copyright of the derived and new portions of this work
 *     (C) 2009 GIST U-VR Lab.
 *
 * This framework is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This framework is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this framework; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * For further information please contact 
 *   Woonhyuk Baek
 *   <windage@live.com>
 *   GIST U-VR Lab.
 *   Department of Information and Communication
 *   Gwangju Institute of Science and Technology
 *   1, Oryong-dong, Buk-gu, Gwangju
 *   South Korea
 * ========================================================================
 ** @author   Woonhyuk Baek
 * ======================================================================== */

#include "Algorithms/EPnPRANSACestimator.h"
using namespace windage;
using namespace windage::Algorithms;

#include "Algorithms/epnp/epnp.h"

int RANSACUpdateNumIters(double p, double ep, int model_points, int max_iters)
{
	double num, denom;
	p = MAX(p, 0.);
    p = MIN(p, 1.);
    ep = MAX(ep, 0.);
    ep = MIN(ep, 1.);

    // avoid inf's & nan's
    num = MAX(1. - p, DBL_MIN);
    denom = 1. - pow(1. - ep,model_points);
	num = log(num);
    denom = log(denom);
    
	int result = denom >= 0 || -num >= max_iters*(-denom) ? max_iters : cvRound(num/denom);
	return result;
}

bool EPnPRANSACestimator::Calculate()
{
	if(this->cameraParameter == NULL)
		return false;
	const int SAMPLE_SIZE = 5;
	int n = (int)this->referencePoints->size();
	if(n < SAMPLE_SIZE)
		return false;
	if(n != (int)this->scenePoints->size())
		return false;

	double fx = this->cameraParameter->GetParameters()[0];
	double fy = this->cameraParameter->GetParameters()[1];
	double cx = this->cameraParameter->GetParameters()[2];
	double cy = this->cameraParameter->GetParameters()[3];

	int idx[SAMPLE_SIZE];
	CvRNG rng = cvRNG(cvGetTickCount());
	int pre_inliers = 0;

	windage::Calibration tempCalibration;
	tempCalibration.Initialize(fx, fy, cx, cy);
	
	// Pose estimation using PnP alogrithm from EPFL
	epnp* _epnp = new epnp;
	_epnp->set_internal_parameters(cx, cy, fx, fy);

	std::vector<bool> inliers_checker;		inliers_checker.resize(n);
	std::vector<bool> pre_inlier_checker;	pre_inlier_checker.resize(n);

	int iter = 0;
	int max_iters = this->maxIteration;
	while(iter<max_iters)
	{
		// sampling
		for(int i=0; i<SAMPLE_SIZE; i++)
		{
			int tempIndex = 0;
			bool found = true;
			while(found)
			{
				tempIndex = cvRandInt(&rng) % n;
				found = false;
				for(int j=0; j<i; j++)
				{
					if(idx[j] == tempIndex)
						found = true;
				}
			}
			idx[i] = tempIndex;
		}

		// estimation
		_epnp->set_maximum_number_of_correspondences(SAMPLE_SIZE);
		_epnp->reset_correspondences();

		for(int i=0; i<SAMPLE_SIZE; i++)
		{
			windage::Vector3 ref = (*this->referencePoints)[idx[i]].GetPoint();
			windage::Vector3 sce = (*this->scenePoints)[idx[i]].GetPoint();

			_epnp->add_correspondence(ref.x, ref.y, ref.z, sce.x, sce.y);
		}

		double _R[3][3], _t[3];
		_epnp->compute_pose(_R, _t);

		double extrinsic[16];
		for(int y=0; y<3; y++)
		{
			for(int x=0; x<3; x++)
			{
				extrinsic[y*4+x] = _R[y][x];
			}
			extrinsic[y*4+3] = _t[y];
		}
		extrinsic[12] = extrinsic[13] = extrinsic[14] = 0.0;
		extrinsic[15] = 1.0;

		tempCalibration.SetExtrinsicMatrix(extrinsic);

		//count inlier
		int num_inliers = 0;
		for(int i=0; i<n; i++)
		{
			windage::Vector3 ref = (*this->referencePoints)[i].GetPoint();
			windage::Vector3 sce = (*this->scenePoints)[i].GetPoint();

			CvPoint projectionPt = tempCalibration.ConvertWorld2Image(ref.x, ref.y, ref.z);
			windage::Vector2 scePt = windage::Vector2(sce.x, sce.y);
			windage::Vector2 projPt = windage::Vector2(projectionPt.x, projectionPt.y);

			inliers_checker[i] = false;
			if(scePt.getDistance(projPt) < this->reprojectionError)
			{
				num_inliers++;
				inliers_checker[i] = true;
			}
		}

		if(num_inliers > pre_inliers)
		{
			pre_inliers = num_inliers;
			for(int i=0; i<n; i++)
			{
				pre_inlier_checker[i] = inliers_checker[i];
			}
			max_iters = RANSACUpdateNumIters(this->confidence, (double)(n - num_inliers)/(double)n, SAMPLE_SIZE, max_iters);
		}
		
		iter++;
	}

	// update pose using inlers	
	_epnp->set_maximum_number_of_correspondences(pre_inliers);
	_epnp->reset_correspondences();
	for(int i=0; i<n; i++)
	{
		if(pre_inlier_checker[i])
		{
			double _X, _Y, _Z, _u, _v;
			_X = (*this->referencePoints)[i].GetPoint().x;
			_Y = (*this->referencePoints)[i].GetPoint().y;
			_Z = (*this->referencePoints)[i].GetPoint().z;
			_u = (*this->scenePoints)[i].GetPoint().x;
			_v = (*this->scenePoints)[i].GetPoint().y;

			_epnp->add_correspondence(_X, _Y, _Z, _u, _v);
		}
	}

	// compute pose
	double _R[3][3], _t[3];
	double _rerror  = _epnp->compute_pose(_R, _t);
	delete _epnp;

	double extrinsic[16];
	for(int y=0; y<3; y++)
	{
		for(int x=0; x<3; x++)
		{
			extrinsic[y*4+x] = _R[y][x];
		}
		extrinsic[y*4+3] = _t[y];
	}
	extrinsic[12] = extrinsic[13] = extrinsic[14] = 0.0;
	extrinsic[15] = 1.0;

	this->cameraParameter->SetExtrinsicMatrix(extrinsic);

	// count inlier
	int num_inliers = 0;
	for(int i=0; i<n; i++)
	{
		windage::Vector3 ref = (*this->referencePoints)[i].GetPoint();
		windage::Vector3 sce = (*this->scenePoints)[i].GetPoint();

		CvPoint projectionPt = this->cameraParameter->ConvertWorld2Image(ref.x, ref.y, ref.z);
		windage::Vector2 scePt = windage::Vector2(sce.x, sce.y);
		windage::Vector2 projPt = windage::Vector2(projectionPt.x, projectionPt.y);

		if(scePt.getDistance(projPt) < this->reprojectionError)
		{
			num_inliers++;
			(*this->referencePoints)[i].SetOutlier(false);
			(*this->scenePoints)[i].SetOutlier(false);
		}
		else
		{
			(*this->referencePoints)[i].SetOutlier(true);
			(*this->scenePoints)[i].SetOutlier(true);
		}
	}

	return true;
}