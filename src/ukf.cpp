#include "ukf.h"
#include "tools.h"
#include "Eigen/Dense"
#include <iostream>

using namespace std;
using Eigen::MatrixXd;
using Eigen::VectorXd;
using std::vector;

/**
 * Initializes Unscented Kalman filter
 */
UKF::UKF() {
  
  //initially set to false, set to true in first call of ProcessMeasurement
  is_initialized_ = false;

  // if this is false, laser measurements will be ignored (except during init)
  use_laser_ = true;

  // if this is false, radar measurements will be ignored (except during init)
  use_radar_ = true;
  
  // time when the state is true, in us
  time_us_ = 0;

  // initial state vector -> to tune
  x_ = VectorXd(5);
  x_ << 1, 1, 9, 0, 0;

  // initial covariance matrix -> to tune
  P_ = MatrixXd(5, 5);
  P_ << 0.5, 0, 0, 0, 0,
        0, 0.5, 0, 0, 0.5,
        0, 0, 0.5, 0, 0,
        0, 0, 0, 0.5, 0,
        0, 0.5, 0, 0, 0.5;
  
  // Process noise standard deviation longitudinal acceleration in m/s^2 -> to tune
  std_a_ = 0.5;

  // Process noise standard deviation yaw acceleration in rad/s^2 -> to tune
  std_yawdd_ = 2;

  // Laser measurement noise standard deviation position1 in m
  std_laspx_ = 0.15;

  // Laser measurement noise standard deviation position2 in m
  std_laspy_ = 0.15;

  // Radar measurement noise standard deviation radius in m
  std_radr_ = 0.3;

  // Radar measurement noise standard deviation angle in rad
  std_radphi_ = 0.03;

  // Radar measurement noise standard deviation radius change in m/s
  std_radrd_ = 0.3;

  // State dimension
  n_x_ = 5;
  
  // Augmented state dimension
  n_aug_ = 7;
  
  // Sigma point spreading parameter
  lambda_ = 3 - n_aug_;
  
  // Weights of sigma points
  weights_ = VectorXd(2 * n_aug_ + 1);
  double weight_0 = lambda_ /(lambda_ + n_aug_);
  weights_(0) = weight_0;
  for (int i=1; i< 2 * n_aug_ + 1; i++) {
    double weight = 0.5/(n_aug_ + lambda_);
    weights_(i) = weight;
  }
  
  // the current NIS for radar
  NIS_radar_ = 0;
  
  // the current NIS for laser
  NIS_laser_ = 0;
  
  // predicted sigma points matrix
  Xsig_pred_ = MatrixXd(n_x_, 2 * n_aug_ + 1);
  
}

UKF::~UKF() {}

/**
 * @param {MeasurementPackage} meas_package The latest measurement data of
 * either radar or laser.
 */
void UKF::ProcessMeasurement(MeasurementPackage meas_package) {
  
  /*****************************************************************************
   *  Initialization
   ****************************************************************************/
  
  if (!is_initialized_) {
    
    // first measurement
    if (meas_package.sensor_type_ == MeasurementPackage::RADAR) {
      float ro = meas_package.raw_measurements_(0);
      float phi = meas_package.raw_measurements_(1);
      x_(0) = ro * cos(phi);
      x_(1) = ro * sin(phi);
    }
    else if (meas_package.sensor_type_ == MeasurementPackage::LASER) {
      x_(0) = meas_package.raw_measurements_(0);
      x_(1) = meas_package.raw_measurements_(1);
    }
    
    // done initializing, no need to predict or update
    is_initialized_ = true;
    time_us_ = meas_package.timestamp_;
    return;
  }
  
  /*****************************************************************************
   *  Prediction
   ****************************************************************************/
  
  // compute the time elapsed between the current and previous measurements
  float dt = (meas_package.timestamp_ - time_us_) / 1000000.0;
  time_us_ = meas_package.timestamp_;
  
  Prediction(dt);
  
  /*****************************************************************************
   *  Update
   ****************************************************************************/
  
  if (meas_package.sensor_type_ == MeasurementPackage::RADAR) {
    // Radar updates
    UpdateRadar(meas_package);
    
  }else {
    // Lidar updates
    UpdateLidar(meas_package);
  }
  
  // print the output
  //cout << "x_ = " << x_ << endl;
  //cout << "P_ = " << P_ << endl;
}

/**
 * Predicts sigma points, the state, and the state covariance matrix.
 * @param {double} delta_t the change in time (in seconds) between the last
 * measurement and this one.
 */
void UKF::Prediction(double delta_t) {
  
  //**** generate sigma points ******************************
  
  // create augmented mean vector
  VectorXd x_aug = VectorXd(7);
  
  // create augmented state covariance
  MatrixXd P_aug = MatrixXd(7, 7);
  
  // create sigma point matrix
  MatrixXd Xsig_aug = MatrixXd(n_aug_, 2 * n_aug_ + 1);
  
  // create augmented mean state
  x_aug.head(5) = x_;
  x_aug(5) = 0;
  x_aug(6) = 0;
  
  // create augmented covariance matrix
  P_aug.fill(0.0);
  P_aug.topLeftCorner(5,5) = P_;
  P_aug(5,5) = std_a_ * std_a_;
  P_aug(6,6) = std_yawdd_ * std_yawdd_;
  
  // create square root matrix
  MatrixXd L = P_aug.llt().matrixL();
  
  // create augmented sigma points
  double const sqrt_lambda = sqrt(lambda_ + n_aug_);
  Xsig_aug.col(0)  = x_aug;
  for (int i = 0; i < n_aug_; i++)
  {
    Xsig_aug.col(i+1)       = x_aug + sqrt_lambda * L.col(i);
    Xsig_aug.col(i+1+n_aug_) = x_aug - sqrt_lambda * L.col(i);
  }
  
  //**** predict sigma points ******************************
  
  for (int i = 0; i < 2 * n_aug_ + 1; i++)
  {
    // extract values for better readability
    double const p_x = Xsig_aug(0,i);
    double const p_y = Xsig_aug(1,i);
    double const v = Xsig_aug(2,i);
    double const yaw = Xsig_aug(3,i);
    double const yawd = Xsig_aug(4,i);
    double const nu_a = Xsig_aug(5,i);
    double const nu_yawdd = Xsig_aug(6,i);
    
    // predicted state values
    double px_p, py_p;
    
    // avoid division by zero
    if (fabs(yawd) > 0.001) {
      px_p = p_x + v/yawd * (sin(yaw + yawd * delta_t) - sin(yaw));
      py_p = p_y + v/yawd * (cos(yaw) - cos(yaw + yawd * delta_t));
    }
    else {
      px_p = p_x + v * delta_t * cos(yaw);
      py_p = p_y + v * delta_t * sin(yaw);
    }
    
    double v_p = v;
    double yaw_p = yaw + yawd * delta_t;
    double yawd_p = yawd;
    
    // add noise
    px_p = px_p + 0.5 * nu_a * delta_t * delta_t * cos(yaw);
    py_p = py_p + 0.5 * nu_a * delta_t * delta_t * sin(yaw);
    v_p = v_p + nu_a * delta_t;
    
    yaw_p = yaw_p + 0.5 * nu_yawdd * delta_t * delta_t;
    yawd_p = yawd_p + nu_yawdd * delta_t;
    
    // write predicted sigma point into right column
    Xsig_pred_(0,i) = px_p;
    Xsig_pred_(1,i) = py_p;
    Xsig_pred_(2,i) = v_p;
    Xsig_pred_(3,i) = yaw_p;
    Xsig_pred_(4,i) = yawd_p;
    
  }

  //**** predict state and covariance matrix ******************************
  
  // predicted state mean
  x_.fill(0.0);
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {  //iterate over sigma points
    x_ = x_ + weights_(i) * Xsig_pred_.col(i);
  }
  
  // predicted state covariance matrix
  P_.fill(0.0);
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {  //iterate over sigma points
    
    // state difference
    VectorXd x_diff = Xsig_pred_.col(i) - x_;
    // angle normalization
    while (x_diff(3)> M_PI) x_diff(3)-= 2.*M_PI;
    while (x_diff(3)<-M_PI) x_diff(3)+= 2.*M_PI;
    
    P_ = P_ + weights_(i) * x_diff * x_diff.transpose();
  }
  
}

/**
 * Updates the state and the state covariance matrix using a laser measurement.
 * @param {MeasurementPackage} meas_package
 */
void UKF::UpdateLidar(MeasurementPackage meas_package) {
    
  // set measurement dimension, lidar can measure px, and py
  int n_z = 2;
  
  // add measurement noise covariance matrix
  MatrixXd R = MatrixXd(n_z, n_z);
  R <<  std_laspx_ * std_laspx_, 0,
        0, std_laspy_  * std_laspy_;
  
  // add measurement matrix
  MatrixXd H_laser = MatrixXd(n_z, n_x_);
  H_laser << 1, 0, 0, 0, 0,
             0, 1, 0, 0, 0;
  
  VectorXd z_pred = H_laser * x_;
  VectorXd y = meas_package.raw_measurements_ - z_pred;
  MatrixXd Ht = H_laser.transpose();
  MatrixXd S = H_laser * P_ * Ht + R;
  MatrixXd Si = S.inverse();
  MatrixXd K = P_ * Ht * Si;
  
  // update new estimate
  x_ = x_ + (K * y);
  MatrixXd I = MatrixXd::Identity(n_x_, n_x_);
  P_ = (I - K * H_laser) * P_;
  
  // calculate the lidar NIS
  NIS_laser_ = y.transpose() * Si * y;
  //if(NIS_laser_ > 5.991){
  //  cout << "Lidar NIS = " << NIS_laser_ << endl;
  //}
  
}

/**
 * Updates the state and the state covariance matrix using a radar measurement.
 * @param {MeasurementPackage} meas_package
 */
void UKF::UpdateRadar(MeasurementPackage meas_package) {
  
  // set measurement dimension, radar can measure r, phi, and r_dot
  int n_z = 3;
  
  // create matrix for sigma points in measurement space
  MatrixXd Zsig = MatrixXd(n_z, 2 * n_aug_ + 1);
  
  // create matrix for cross correlation Tc
  MatrixXd Tc = MatrixXd(n_x_, n_z);
  
  // **** transform sigma points into measurement space *************************
  
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {
    
    // extract values for better readibility
    double p_x = Xsig_pred_(0,i);
    double p_y = Xsig_pred_(1,i);
    double v  = Xsig_pred_(2,i);
    double yaw = Xsig_pred_(3,i);
    
    double v1 = cos(yaw)*v;
    double v2 = sin(yaw)*v;
    
    // measurement model
    Zsig(0,i) = sqrt(p_x * p_x + p_y * p_y);                          //r
    if (fabs(Zsig(0,i)) < 0.001){
      // avoid division by 0
      Zsig(0,i) =0.001;
    };
    // TODO: if p_y = 0 and p_x = 0
    Zsig(1,i) = atan2(p_y, p_x);                                      //phi
    Zsig(2,i) = (p_x * v1 + p_y * v2) / Zsig(0,i) ;                   //r_dot
  }
  
  // mean predicted measurement
  VectorXd z_pred = VectorXd(n_z);
  z_pred.fill(0.0);
  for (int i=0; i < 2 * n_aug_ + 1; i++) {
    z_pred = z_pred + weights_(i) * Zsig.col(i);
  }
  
  // store z_diff to avoid repeatitive computations
  MatrixXd z_diff = MatrixXd(n_z, 2 * n_aug_ + 1);
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {
    // residual
    VectorXd z_diff_tmp = Zsig.col(i) - z_pred;
    // angle normalization
    while (z_diff_tmp(1)> M_PI) z_diff_tmp(1)-= 2.*M_PI;
    while (z_diff_tmp(1)<-M_PI) z_diff_tmp(1)+= 2.*M_PI;
    // store to z_diff
    z_diff.col(i) = z_diff_tmp;
  }
  
  // measurement covariance matrix S
  MatrixXd S = MatrixXd(n_z,n_z);
  S.fill(0.0);
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {
    S = S + weights_(i) * z_diff.col(i) * z_diff.col(i).transpose();
  }
  
  // add measurement noise covariance matrix
  MatrixXd R = MatrixXd(n_z,n_z);
  R <<  std_radr_ * std_radr_, 0, 0,
        0, std_radphi_ * std_radphi_, 0,
        0, 0, std_radrd_  * std_radrd_;
  S = S + R;
  
  // **** update state mean and covariance matrix *************************
  
  // calculate cross correlation matrix
  Tc.fill(0.0);
  for (int i = 0; i < 2 * n_aug_ + 1; i++) {
    
    // state difference
    VectorXd x_diff = Xsig_pred_.col(i) - x_;
    // angle normalization
    while (x_diff(3)> M_PI) x_diff(3)-= 2.*M_PI;
    while (x_diff(3)<-M_PI) x_diff(3)+= 2.*M_PI;
    
    Tc = Tc + weights_(i) * x_diff * z_diff.col(i).transpose();
  }
  
  // Kalman gain K;
  MatrixXd K = Tc * S.inverse();
  
  // residual
  VectorXd z_diff_real = meas_package.raw_measurements_ - z_pred;
  
  // angle normalization
  while (z_diff_real(1)> M_PI) z_diff_real(1)-= 2.*M_PI;
  while (z_diff_real(1)<-M_PI) z_diff_real(1)+= 2.*M_PI;
  
  // update state mean and covariance matrix
  x_ = x_ + K * z_diff_real;
  P_ = P_ - K * S * K.transpose();
  
  // **** calculate the radar NIS ********************************************
  NIS_radar_ = z_diff_real.transpose() * S.inverse() * z_diff_real;
  //if(NIS_radar_ > 7.815){
  //  cout << "Radar NIS = " << NIS_radar_ << endl;
  //}

}
