//////////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) Los Alamos National Security, LLC.  This file is part of the
//  Tusas code (LA-CC-17-001) and is subject to the revised BSD license terms
//  in the LICENSE file found in the top-level directory of this distribution.
//
//////////////////////////////////////////////////////////////////////////////


#ifndef NOX_THYRA_MODEL_EVALUATOR_TPETRA_DEF_HPP
#define NOX_THYRA_MODEL_EVALUATOR_TPETRA_DEF_HPP

#include <Teuchos_DefaultComm.hpp>
#include <Teuchos_Describable.hpp>
#include <Teuchos_ArrayViewDecl.hpp>
#include <Teuchos_TimeMonitor.hpp>

//#include <Kokkos_View.hpp> 	
#include <Kokkos_Vector.hpp>

#include <Tpetra_Core.hpp>
#include <Tpetra_Map.hpp>
#include <Tpetra_Map_decl.hpp>
#include <Tpetra_Import.hpp>

#include <Thyra_TpetraThyraWrappers.hpp>
#include <Thyra_VectorBase.hpp>

template<class Scalar>
Teuchos::RCP<ModelEvaluatorTPETRA<Scalar> >
modelEvaluatorTPETRA( const Teuchos::RCP<const Epetra_Comm>& comm,
			Mesh *mesh,
			 Teuchos::ParameterList plist
			 )
{
  return Teuchos::rcp(new ModelEvaluatorTPETRA<Scalar>(mesh,plist));
}

// Constructor

template<class Scalar>
ModelEvaluatorTPETRA<Scalar>::
ModelEvaluatorTPETRA( const Teuchos::RCP<const Epetra_Comm>& comm,
			Mesh *mesh,
			 Teuchos::ParameterList plist 
		     ) :
  paramList(plist),
  mesh_(mesh),
  Comm(comm)
{
  dt_ = paramList.get<double> (TusasdtNameString);
  t_theta_ = paramList.get<double> (TusasthetaNameString);

  set_test_case();

  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 
  //comm_->describe(*(Teuchos::VerboseObjectBase::getDefaultOStream()),Teuchos::EVerbosityLevel::VERB_EXTREME );
  
  //mesh_ = Teuchos::rcp(new Mesh(*mesh));
  mesh_->compute_nodal_adj(); 
  std::vector<int> node_num_map(mesh_->get_node_num_map());
  
  std::vector<int> my_global_nodes(numeqs_*node_num_map.size());
  for(int i = 0; i < node_num_map.size(); i++){    
    for( int k = 0; k < numeqs_; k++ ){
      my_global_nodes[numeqs_*i+k] = numeqs_*node_num_map[i]+k;
    }
  }

  const global_size_t numGlobalEntries = Teuchos::OrdinalTraits<Tpetra::global_size_t>::invalid();
  const global_ordinal_type indexBase = 0;

  Teuchos::ArrayView<int> AV(my_global_nodes);

  x_overlap_map_ = Teuchos::rcp(new map_type(numGlobalEntries,
					     AV,
					     indexBase,
					     comm_
					     ));

  //x_overlap_map_ ->describe(*(Teuchos::VerboseObjectBase::getDefaultOStream()),Teuchos::EVerbosityLevel::VERB_DEFAULT );

  x_owned_map_ = Teuchos::rcp(new map_type(*(createOneToOne(x_overlap_map_))));
  //x_owned_map_ ->describe(*(Teuchos::VerboseObjectBase::getDefaultOStream()),Teuchos::EVerbosityLevel::VERB_DEFAULT );
  
  importer_ = Teuchos::rcp(new import_type(x_owned_map_, x_overlap_map_));
  //exporter_ = Teuchos::rcp(new export_type(x_owned_map_, x_overlap_map_));
  exporter_ = Teuchos::rcp(new export_type(x_overlap_map_, x_owned_map_));
  
  num_owned_nodes_ = x_owned_map_->getNodeNumElements()/numeqs_;
  num_overlap_nodes_ = x_overlap_map_->getNodeNumElements()/numeqs_;

  Teuchos::ArrayView<int> NV(node_num_map);
  node_overlap_map_ = Teuchos::rcp(new map_type(numGlobalEntries,
						NV,
						indexBase,
						comm_
						));

  //cn we could store previous time values in a multivector
  u_old_ = Teuchos::rcp(new vector_type(x_owned_map_));

  x_ = Teuchos::rcp(new vector_type(node_overlap_map_));
  y_ = Teuchos::rcp(new vector_type(node_overlap_map_));
  z_ = Teuchos::rcp(new vector_type(node_overlap_map_));
  ArrayRCP<scalar_type> xv = x_->get1dViewNonConst();
  ArrayRCP<scalar_type> yv = y_->get1dViewNonConst();
  ArrayRCP<scalar_type> zv = z_->get1dViewNonConst();
  const size_t localLength = node_overlap_map_->getNodeNumElements();
  for (size_t nn=0; nn < localLength; nn++) {
    xv[nn] = mesh_->get_x(nn);
    yv[nn] = mesh_->get_y(nn);
    zv[nn] = mesh_->get_z(nn);
  }
 
  x_space_ = Thyra::createVectorSpace<scalar_type>(x_owned_map_);
  f_space_ = x_space_;
  //x0_ = Thyra::createMember(x_space_);
  x0_ = Teuchos::rcp(new vector_type(x_owned_map_));
  x0_->putScalar(Teuchos::ScalarTraits<scalar_type>::zero());

  Thyra::ModelEvaluatorBase::InArgsSetup<scalar_type> inArgs;
  inArgs.setModelEvalDescription(this->description());
  inArgs.setSupports(Thyra::ModelEvaluatorBase::IN_ARG_x);
  prototypeInArgs_ = inArgs;

  Thyra::ModelEvaluatorBase::OutArgsSetup<scalar_type> outArgs;
  outArgs.setModelEvalDescription(this->description());
  outArgs.setSupports(Thyra::ModelEvaluatorBase::OUT_ARG_f);
  //outArgs.setSupports(Thyra::ModelEvaluatorBase::OUT_ARG_W_prec);
  prototypeOutArgs_ = outArgs;
  nominalValues_ = inArgs;
  //nominalValues_.set_x(x0_);;
  nominalValues_.set_x(Thyra::createVector(x0_, x_space_));
  time_=0.;
  
  ts_time_import= Teuchos::TimeMonitor::getNewTimer("Total Import Time");
  ts_time_resfill= Teuchos::TimeMonitor::getNewTimer("Total Residual Fill Time");
  //ts_time_precfill= Teuchos::TimeMonitor::getNewTimer("Total Preconditioner Fill Time");
  ts_time_nsolve= Teuchos::TimeMonitor::getNewTimer("Total Nonlinear Solver Time");

  //HACK
  //cn 8-28-18 currently elem_color takes an epetra_mpi_comm....
  //there are some epetra_maps and a routine that does mpi calls for off proc comm const 
  //Comm = Teuchos::rcp(new Epetra_MpiComm( MPI_COMM_WORLD ));
  Elem_col = Teuchos::rcp(new elem_color(Comm,mesh));

  init_nox();

}

template<class Scalar>
void ModelEvaluatorTPETRA<Scalar>::set_x0(const Teuchos::ArrayView<const Scalar> &x0_in)
{
#ifdef TEUCHOS_DEBUG
  TEUCHOS_ASSERT_EQUALITY(x_space_->dim(), x0_in.size());
#endif
  x0_->get1dViewNonConst()().assign(x0_in);
}

template<class Scalar>
void ModelEvaluatorTPETRA<Scalar>::evalModelImpl(
  const Thyra::ModelEvaluatorBase::InArgs<Scalar> &inArgs,
  const Thyra::ModelEvaluatorBase::OutArgs<Scalar> &outArgs
  ) const
{  

  //cn the easiest way probably to do the sum into off proc nodes is to load a 
  //vector(overlap_map) the export with summation to the f_vec(owned_map)
  //after summing into. ie import is uniquely-owned to multiply-owned
  //export is multiply-owned to uniquely-owned

  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 

  typedef Thyra::TpetraOperatorVectorExtraction<Scalar,int> ConverterT;

  const Teuchos::RCP<const vector_type > x_vec =
    ConverterT::getConstTpetraVector(inArgs.get_x());

  Teuchos::RCP<vector_type > u = Teuchos::rcp(new vector_type(x_overlap_map_));
  Teuchos::RCP<vector_type > uold = Teuchos::rcp(new vector_type(x_overlap_map_));
  {
    Teuchos::TimeMonitor ImportTimer(*ts_time_import);
    u->doImport(*x_vec,*importer_,Tpetra::INSERT);
    uold->doImport(*u_old_,*importer_,Tpetra::INSERT);
  }

  if (nonnull(outArgs.get_f())){

    const RCP<vector_type> f_vec =
      ConverterT::getTpetraVector(outArgs.get_f());

    Teuchos::RCP<vector_type> f_overlap = Teuchos::rcp(new vector_type(x_overlap_map_));
    f_vec->scale(0.);
    f_overlap->scale(0.);
    Teuchos::TimeMonitor ResFillTimer(*ts_time_resfill);  
    const int blk = 0;
    const int n_nodes_per_elem = mesh_->get_num_nodes_per_elem_in_blk(blk);//shared
    //std::cout<<num_color<<" "<<colors[0].size()<<" "<<(Elem_col->get_color(0)).size()<<std::endl;
    OMPBasisLQuad B;

    Kokkos::vector<int> meshc(((mesh_->connect)[0]).size());
    for(int i = 0; i<((mesh_->connect)[0]).size(); i++) meshc[i]=(mesh_->connect)[0][i];

    Kokkos::vector<int> meshn = ((mesh_->get_node_num_map()).size());
    for(int i = 0; i<((mesh_->get_node_num_map())).size(); i++) meshn[i]=(mesh_->get_node_num_map())[i];

    double dt = dt_; //cuda 8 lambdas dont capture private data

    const int num_color = Elem_col->get_num_color();

    auto u_view = u->getLocalView<Kokkos::DefaultExecutionSpace>();
    auto uold_view = uold->getLocalView<Kokkos::DefaultExecutionSpace>();
    auto x_view = x_->getLocalView<Kokkos::DefaultExecutionSpace>();
    auto y_view = y_->getLocalView<Kokkos::DefaultExecutionSpace>();
    auto z_view = z_->getLocalView<Kokkos::DefaultExecutionSpace>();
    
    auto f_view = f_overlap->getLocalView<Kokkos::DefaultExecutionSpace>();
    
    
//     auto u_1d = Kokkos::subview (u_view, Kokkos::ALL (), 0);
//     auto uold_1d = Kokkos::subview (uold_view, Kokkos::ALL (), 0);
//     auto x_1d = Kokkos::subview (x_view, Kokkos::ALL (), 0);
//     auto y_1d = Kokkos::subview (y_view, Kokkos::ALL (), 0);
//     auto z_1d = Kokkos::subview (z_view, Kokkos::ALL (), 0);

    auto f_1d = Kokkos::subview (f_view, Kokkos::ALL (), 0);

    //using RandomAccess should give better memory performance on better than tesla gpus (guido is tesla and does not show performance increase)
    //this will utilize texture memory not available on tesla or earlier gpus
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> x_1dra = Kokkos::subview (x_view, Kokkos::ALL (), 0);
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> y_1dra = Kokkos::subview (y_view, Kokkos::ALL (), 0);
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> z_1dra = Kokkos::subview (z_view, Kokkos::ALL (), 0);
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> u_1dra = Kokkos::subview (u_view, Kokkos::ALL (), 0);
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> uold_1dra = Kokkos::subview (uold_view, Kokkos::ALL (), 0);


    for(int c = 0; c < num_color; c++){
      //std::vector<int> elem_map = colors[c];
      std::vector<int> elem_map = Elem_col->get_color(c);

      const int num_elem = elem_map.size();

      Kokkos::vector<int> elem_map_k(num_elem);
      for(int i = 0; i<num_elem; i++) {
	elem_map_k[i] = elem_map[i];
	//std::cout<<comm_->getRank()<<" "<<c<<" "<<i<<" "<<elem_map_k[i]<<std::endl;
      }
      //exit(0);

      //#pragma omp parallel for
      //for (int ne=0; ne < num_elem; ne++) { 
      Kokkos::parallel_for(num_elem,KOKKOS_LAMBDA(const size_t ne){

	const int elem = elem_map_k[ne];
	double xx[4];
	double yy[4];
	double zz[4];
	double uu[4];
	double uu_old[4];
	for(int k = 0; k < n_nodes_per_elem; k++){

	  //const int nodeid = mesh_->get_node_id(blk, elem, k);//cn this is the local id
	  const int nodeid = meshc[elem*n_nodes_per_elem+k];
	  
	  xx[k] = x_1dra(nodeid);
	  yy[k] = y_1dra(nodeid);
	  zz[k] = z_1d(nodeid);
	  uu[k] = u_1dra(nodeid); 
	  uu_old[k] = uold_1dra(nodeid);
	}//k

	for (int i=0; i< n_nodes_per_elem; i++) {//i
	  for(int gp=0; gp < 4; gp++) {//gp
	    //B.getBasis(gp, xx, yy, zz, uu, uu_old, uu_old_old);
	    const double dxdxi  = .25*( (xx[1]-xx[0])*(1.-B.eta[gp])+(xx[2]-xx[3])*(1.+B.eta[gp]) );
	    const double dxdeta = .25*( (xx[3]-xx[0])*(1.- B.xi[gp])+(xx[2]-xx[1])*(1.+ B.xi[gp]) );
	    const double dydxi  = .25*( (yy[1]-yy[0])*(1.- B.eta[gp])+(yy[2]-yy[3])*(1.+ B.eta[gp]) );
	    const double dydeta = .25*( (yy[3]-yy[0])*(1.-  B.xi[gp])+(yy[2]-yy[1])*(1.+  B.xi[gp]) );
	    const double jac = dxdxi * dydeta - dxdeta * dydxi;
	    const double dxidx = dydeta / jac;
	    const double dxidy = -dxdeta / jac;
	    const double detadx = -dydxi / jac;
	    const double detady = dxdxi / jac;
	    double uuu = 0.;
	    double uuuold = 0.;
	    double dudx = 0.;
	    double dudy = 0.;
	    for (int ii=0; ii < n_nodes_per_elem; ii++) {
	      uuu += uu[ii] * B.phi1[ii][gp];
	      uuuold +=uu_old[ii] * B.phi1[ii][gp];
	      dudx += uu[ii] * (B.dphidxi1[ii][gp]*dxidx+B.dphideta1[ii][gp]*detadx);
	      dudy += uu[ii] * (B.dphidxi1[ii][gp]*dxidy+B.dphideta1[ii][gp]*detady);
	    }//ii
	    const double wt = B.weight[0];
	    const double val = jac*wt*(
			 (uuu-uuuold)/dt*B.phi1[i][gp] 
			 + dudx*(B.dphidxi1[i][gp]*dxidx + B.dphideta1[i][gp]*detadx)
			 + dudy*(B.dphidxi1[i][gp]*dxidy + B.dphideta1[i][gp]*detady)
			 );

	    const int lid = meshc[elem*n_nodes_per_elem+i];

	    f_1d(lid) += val;

	  }//gp
	}//i
	});//parallel_for
	//}//ne

    }//c 
    
    //f_overlap->sync<Kokkos::HostSpace> ();


    {
      Teuchos::TimeMonitor ImportTimer(*ts_time_import);  
      f_vec->doExport(*f_overlap, *exporter_, Tpetra::ADD);
    }


  }//get_f


  //cn it also seems that the boundary condition is done on overlap map as well...



  if (nonnull(outArgs.get_f()) && NULL != dirichletfunc_){
    const RCP<vector_type> f_vec =
      ConverterT::getTpetraVector(outArgs.get_f());

    std::vector<int> node_num_map(mesh_->get_node_num_map());
    std::map<int,DBCFUNC>::iterator it;
    
    Teuchos::RCP<vector_type> f_overlap = Teuchos::rcp(new vector_type(x_overlap_map_));
    {
      Teuchos::TimeMonitor ImportTimer(*ts_time_import);
      f_overlap->doImport(*f_vec,*importer_,Tpetra::INSERT);
    }

    //u is already imported to overlap_map here
    auto u_view = u->getLocalView<Kokkos::DefaultExecutionSpace>();
    auto f_view = f_overlap->getLocalView<Kokkos::DefaultExecutionSpace>();
    
    
    //auto u_1d = Kokkos::subview (u_view, Kokkos::ALL (), 0);
    Kokkos::View<const double*, Kokkos::MemoryTraits<Kokkos::RandomAccess>> u_1dra = Kokkos::subview (u_view, Kokkos::ALL (), 0);
    auto f_1d = Kokkos::subview (f_view, Kokkos::ALL (), 0);
	

    for( int k = 0; k < numeqs_; k++ ){
      for(it = (*dirichletfunc_)[k].begin();it != (*dirichletfunc_)[k].end(); ++it){
	const int ns_id = it->first;
	const int num_node_ns = mesh_->get_node_set(ns_id).size();


	size_t ns_size = (mesh_->get_node_set(ns_id)).size();
	Kokkos::View <double*> node_set_view("nsv",ns_size);
	for (size_t i = 0; i < ns_size; ++i) {
	  node_set_view(i) = (mesh_->get_node_set(ns_id))[i];
        }

 	//for ( int j = 0; j < num_node_ns; j++ ){
	Kokkos::parallel_for(num_node_ns,KOKKOS_LAMBDA (const size_t& j){
			       const int lid = node_set_view(j);//could use Kokkos::vector here...
			       //std::cout<<comm_->getRank()<<":: "<<j<<"  "<<lid<<"  "<<x_overlap_map_->getGlobalElement(lid)<<std::endl;
			       const double val1 = 0.;
			       const double val = u_1dra(lid)  - val1;
			       f_1d(lid) = val;
			     });
			       //}//j  

      }//it
    }//k
    {
      Teuchos::TimeMonitor ImportTimer(*ts_time_import);  
      f_vec->doExport(*f_overlap, *exporter_, Tpetra::INSERT);
    }
     
  }//get_f
  //exit(0);
#if 0
  //cn this will be for a get_W_prec() not a get_W_op()
  const RCP<Tpetra::CrsMatrix<scalar_type, int> > W =
    Teuchos::rcp_dynamic_cast<Tpetra::CrsMatrix<scalar_type,int> >(
      ConverterT::getTpetraOperator(outArgs.get_W_op()),
      true
      );
  if (nonnull(W)) {
    W->setAllToscalar_type(ST::zero());
    W->sumIntoGlobalValues(0, tuple<int>(0, 1), tuple<scalar_type>(1.0, 2.0*x[1]));
    W->sumIntoGlobalValues(1, tuple<int>(0, 1), tuple<scalar_type>(2.0*d_*x[0], -d_));
  }
#endif
  return;
}

//====================================================================

template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::init_nox()
{
  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 
  int mypid = comm_->getRank() ;
  if( 0 == mypid )
    std::cout<<std::endl<<"init_nox() started."<<std::endl<<std::endl;

  nnewt_=0;

  ::Stratimikos::DefaultLinearSolverBuilder builder;

  Teuchos::RCP<Teuchos::ParameterList> lsparams =
    Teuchos::rcp(new Teuchos::ParameterList(paramList.sublist(TusaslsNameString)));

  builder.setParameterList(lsparams);

  if( 0 == mypid )
    builder.getParameterList()->print(std::cout);

  Teuchos::RCP< ::Thyra::LinearOpWithSolveFactoryBase<double> >
    lowsFactory = builder.createLinearSolveStrategy("");

  // Setup output stream and the verbosity level
  Teuchos::RCP<Teuchos::FancyOStream>
    out = Teuchos::VerboseObjectBase::getDefaultOStream();
  lowsFactory->setOStream(out);
  lowsFactory->setVerbLevel(Teuchos::VERB_EXTREME);

  this->set_W_factory(lowsFactory);

  // Create the initial guess
  Teuchos::RCP< ::Thyra::VectorBase<double> >
    initial_guess = this->getNominalValues().get_x()->clone_v();

  Thyra::V_S(initial_guess.ptr(),Teuchos::ScalarTraits<double>::one());

  // Create the JFNK operator
  Teuchos::ParameterList printParams;//cn this is empty??? for now
  Teuchos::RCP<NOX::Thyra::MatrixFreeJacobianOperator<double> > jfnkOp =
    Teuchos::rcp(new NOX::Thyra::MatrixFreeJacobianOperator<double>(printParams));

  Teuchos::RCP<Teuchos::ParameterList> jfnkParams = Teuchos::rcp(new Teuchos::ParameterList(paramList.sublist(TusasjfnkNameString)));
  jfnkOp->setParameterList(jfnkParams);
  if( 0 == mypid )
    jfnkParams->print(std::cout);

  Teuchos::RCP< ::Thyra::ModelEvaluator<double> > Model = Teuchos::rcpFromRef(*this);
  // Wrap the model evaluator in a JFNK Model Evaluator
  Teuchos::RCP< ::Thyra::ModelEvaluator<double> > thyraModel =
    Teuchos::rcp(new NOX::MatrixFreeModelEvaluatorDecorator<double>(Model));

  // Wrap the model evaluator in a JFNK Model Evaluator
//   Teuchos::RCP< ::Thyra::ModelEvaluator<double> > thyraModel =
//     Teuchos::rcp(new NOX::MatrixFreeModelEvaluatorDecorator<double>(this));

  //Teuchos::RCP< ::Thyra::PreconditionerBase<double> > precOp = thyraModel->create_W_prec();
  // Create the NOX::Thyra::Group

  bool precon = paramList.get<bool> (TusaspreconNameString);
  Teuchos::RCP<NOX::Thyra::Group> nox_group;
  if(precon){
#if 0
    Teuchos::RCP< ::Thyra::PreconditionerBase<double> > precOp = thyraModel->create_W_prec();
    nox_group =
      Teuchos::rcp(new NOX::Thyra::Group(*initial_guess, thyraModel, jfnkOp, lowsFactory, precOp, Teuchos::null));
#endif
  }
  else {
    nox_group =
      Teuchos::rcp(new NOX::Thyra::Group(*initial_guess, thyraModel, jfnkOp, lowsFactory, Teuchos::null, Teuchos::null));
  }

  nox_group->computeF();

  // VERY IMPORTANT!!!  jfnk object needs base evaluation objects.
  // This creates a circular dependency, so use a weak pointer.
  jfnkOp->setBaseEvaluationToNOXGroup(nox_group.create_weak());

  // Create the NOX status tests and the solver
  // Create the convergence tests
  Teuchos::RCP<NOX::StatusTest::NormF> absresid =
    Teuchos::rcp(new NOX::StatusTest::NormF(1.0e-8));

  double relrestol = 1.0e-6;
  relrestol = paramList.get<double> (TusasnoxrelresNameString);

  Teuchos::RCP<NOX::StatusTest::NormF> relresid = 
    Teuchos::rcp(new NOX::StatusTest::NormF(*nox_group.get(), relrestol));//1.0e-6 for paper

  Teuchos::RCP<NOX::StatusTest::NormWRMS> wrms =
    Teuchos::rcp(new NOX::StatusTest::NormWRMS(1.0e-2, 1.0e-8));
  Teuchos::RCP<NOX::StatusTest::Combo> converged =
    Teuchos::rcp(new NOX::StatusTest::Combo(NOX::StatusTest::Combo::AND));
  //converged->addStatusTest(absresid);
  converged->addStatusTest(relresid);
  //converged->addStatusTest(wrms);

  int maxit = 200;
  maxit = paramList.get<int> (TusasnoxmaxiterNameString);

  Teuchos::RCP<NOX::StatusTest::MaxIters> maxiters =
    Teuchos::rcp(new NOX::StatusTest::MaxIters(maxit));//200

  Teuchos::RCP<NOX::StatusTest::FiniteValue> fv =
    Teuchos::rcp(new NOX::StatusTest::FiniteValue);
  Teuchos::RCP<NOX::StatusTest::Combo> combo =
    Teuchos::rcp(new NOX::StatusTest::Combo(NOX::StatusTest::Combo::OR));
  //combo->addStatusTest(fv);
  combo->addStatusTest(converged);
  combo->addStatusTest(maxiters);

  Teuchos::RCP<Teuchos::ParameterList> nl_params =
    Teuchos::rcp(new Teuchos::ParameterList(paramList.sublist(TusasnlsNameString)));
  if( 0 == mypid )
    nl_params->print(std::cout);
  Teuchos::ParameterList& nlPrintParams = nl_params->sublist("Printing");
  nlPrintParams.set("Output Information",
		  NOX::Utils::OuterIteration  +
		  //                      NOX::Utils::OuterIterationStatusTest +
		  NOX::Utils::InnerIteration +
		    NOX::Utils::Details //+
		    //NOX::Utils::LinearSolverDetails
		    );
  // Create the solver
  solver_ =  NOX::Solver::buildSolver(nox_group, combo, nl_params);

  if( 0 == mypid )
    std::cout<<std::endl<<"init_nox() completed."<<std::endl<<std::endl;
}

template<class Scalar>
void ModelEvaluatorTPETRA<Scalar>::
set_W_factory(const Teuchos::RCP<const ::Thyra::LinearOpWithSolveFactoryBase<Scalar> >& W_factory)
{
  W_factory_ = W_factory;
}

template<class scalar_type>
Thyra::ModelEvaluatorBase::OutArgs<scalar_type>
ModelEvaluatorTPETRA<scalar_type>::createOutArgsImpl() const
{
  return prototypeOutArgs_;
}

template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::advance()
{
  Teuchos::RCP< VectorBase< double > > guess = Thyra::createVector(u_old_,x_space_);
  NOX::Thyra::Vector thyraguess(*guess);//by sending the dereferenced pointer, we instigate a copy rather than a view
  solver_->reset(thyraguess);

  {
    Teuchos::TimeMonitor NSolveTimer(*ts_time_nsolve);

    NOX::StatusTest::StatusType solvStatus = solver_->solve();
    if( !(NOX::StatusTest::Converged == solvStatus)) {
      std::cout<<" NOX solver failed to converge. Status = "<<solvStatus<<std::endl<<std::endl;
      if(200 == paramList.get<int> (TusasnoxmaxiterNameString)) exit(0);
    }
  }
  nnewt_ += solver_->getNumIterations();

  const Thyra::VectorBase<double> * sol = 
    &(dynamic_cast<const NOX::Thyra::Vector&>(
					      solver_->getSolutionGroup().getX()
					      ).getThyraVector()
      );
  Thyra::ConstDetachedSpmdVectorView<double> x_vec(sol->col(0));

  ArrayRCP<scalar_type> uv = u_old_->get1dViewNonConst();
  const size_t localLength = num_owned_nodes_;


  for (int nn=0; nn < localLength; nn++) {//cn figure out a better way here...

    for( int k = 0; k < numeqs_; k++ ){
      uv[numeqs_*nn+k]=x_vec[numeqs_*nn+k];
    }
  }

  time_ +=dt_;
#if 0
  //boost::ptr_vector<error_estimator>::iterator it;
  for(boost::ptr_vector<error_estimator>::iterator it = Error_est.begin();it != Error_est.end();++it){
    //it->test_lapack();
    it->estimate_gradient(u_old_);
    it->estimate_error(u_old_);
  }
    
  postprocess();
#endif
}

template<class scalar_type>
  void ModelEvaluatorTPETRA<scalar_type>::initialize()
{
  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 
  if( 0 == comm_->getRank()) std::cout<<std::endl<<"inititialize started"<<std::endl<<std::endl;
  bool dorestart = paramList.get<bool> (TusasrestartNameString);
  if (!dorestart){ 
    init(u_old_); 
#if 0 
    *u_old_old_ = *u_old_;
#endif  

    int mypid = comm_->getRank();
    int numproc = comm_->getSize();
    
    if( 1 == numproc ){//cn for now
      //if( 0 == mypid ){
      const char *outfilename = "results.e";
      ex_id_ = mesh_->create_exodus(outfilename);
      
    }
    else{
      std::string decompPath="decomp/";
      //std::string pfile = decompPath+std::to_string(mypid+1)+"/results.e."+std::to_string(numproc)+"."+std::to_string(mypid);
      
      std::string mypidstring;
      if ( numproc > 9 && mypid < 10 ){
	mypidstring = std::to_string(0)+std::to_string(mypid);
      }
      else{
	mypidstring = std::to_string(mypid);
      }
      
      std::string pfile = decompPath+"/results.e."+std::to_string(numproc)+"."+mypidstring;
      ex_id_ = mesh_->create_exodus(pfile.c_str());
    }  
    for( int k = 0; k < numeqs_; k++ ){
      mesh_->add_nodal_field((*varnames_)[k]);
    }
    
    output_step_ = 1;
    write_exodus();
  }

#if 0
  else{
    restart(u_old_,u_old_old_);
//     if(1==comm_->MyPID())
//       std::cout<<"Restart unavailable"<<std::endl<<std::endl;
//     exit(0);
    for( int k = 0; k < numeqs_; k++ ){
      mesh_->add_nodal_field((*varnames_)[k]);
    }
  }
#endif    
  if( 0 == comm_->getRank()) std::cout<<std::endl<<"initialize finished"<<std::endl<<std::endl;
}
template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::init(Teuchos::RCP<vector_type> u)
{
  ArrayRCP<scalar_type> uv = u->get1dViewNonConst();
  const size_t localLength = num_owned_nodes_;
  for( int k = 0; k < numeqs_; k++ ){
    //#pragma omp parallel for
    for (size_t nn=0; nn < localLength; nn++) {

      const int gid_node = x_owned_map_->getGlobalElement(nn*numeqs_); 
      const int lid_overlap = (x_overlap_map_->getLocalElement(gid_node))/numeqs_; 

      const double x = mesh_->get_x(lid_overlap);
      const double y = mesh_->get_y(lid_overlap);
      const double z = mesh_->get_z(lid_overlap);
  




      uv[numeqs_*nn+k] = (*initfunc_)[k](x,y,z,k);
      //std::cout<<uv[numeqs_*nn+k]<<" "<<x<<" "<<y<<std::endl;
    }


  }
  //exit(0);
}

template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::set_test_case()
{
  numeqs_ = 1;

  varnames_ = new std::vector<std::string>(numeqs_);
  (*varnames_)[0] = "u";

  initfunc_ = new  std::vector<INITFUNC>(numeqs_);
  (*initfunc_)[0] = &heat::init_heat_test_;
  
  // numeqs_ number of variables(equations) 
  dirichletfunc_ = new std::vector<std::map<int,DBCFUNC>>(numeqs_);
  
  //  cubit nodesets start at 1; exodus nodesets start at 0, hence off by one here
  //               [numeq][nodeset id]
//  [variable index][nodeset index]
  (*dirichletfunc_)[0][0] = &dbc_zero_;							 
  (*dirichletfunc_)[0][1] = &dbc_zero_;						 
  (*dirichletfunc_)[0][2] = &dbc_zero_;						 
  (*dirichletfunc_)[0][3] = &dbc_zero_;
}

template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::write_exodus()
//void ModelEvaluatorNEMESIS<scalar_type>::write_exodus(const int output_step)
{
  update_mesh_data();

  //not sre what the bug is here...
  mesh_->write_exodus(ex_id_,output_step_,time_);
  output_step_++;
}

template<class scalar_type>
int ModelEvaluatorTPETRA<scalar_type>:: update_mesh_data()
{
  //std::cout<<"update_mesh_data()"<<std::endl;

  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 
  //std::vector<int> node_num_map(mesh_->get_node_num_map());

  //cn 8-28-18 we need an overlap map with mpi, since shared nodes live
  // on the decomposed mesh----fix this later
  Teuchos::RCP<vector_type> temp;

  if( 1 == comm_->getSize() ){
    temp = Teuchos::rcp(new vector_type(*u_old_));
  }
  else{
    temp = Teuchos::rcp(new vector_type(x_overlap_map_));//cn might be better to have u_old_ live on overlap map
    temp->doImport(*u_old_, *importer_, Tpetra::INSERT);
  }


  //cn 8-28-18 we need an overlap map with mpi, since shared nodes live
  // on the decomposed mesh----fix this later
  int num_nodes = num_overlap_nodes_;
  std::vector<std::vector<double>> output(numeqs_, std::vector<double>(num_nodes));

  const ArrayRCP<scalar_type> uv = temp->get1dViewNonConst();
  //const size_t localLength = num_owned_nodes_;

  for( int k = 0; k < numeqs_; k++ ){
    //#pragma omp parallel for
    for (int nn=0; nn < num_nodes; nn++) {
      //output[k][nn]=(*temp)[numeqs_*nn+k];
      output[k][nn]=uv[numeqs_*nn+k];
      //std::cout<<uv[numeqs_*nn+k]<<std::endl;
    }
  }
  int err = 0;
  for( int k = 0; k < numeqs_; k++ ){
    mesh_->update_nodal_data((*varnames_)[k], &output[k][0]);
  }
#if 0

  boost::ptr_vector<error_estimator>::iterator it;
  for(it = Error_est.begin();it != Error_est.end();++it){
    it->update_mesh_data();
  }

  boost::ptr_vector<post_process>::iterator itp;
  for(itp = post_proc.begin();itp != post_proc.end();++itp){
    itp->update_mesh_data();
    itp->update_scalar_data(time_);
  }

  delete temp;
#endif

  Elem_col->update_mesh_data();

  return err;
}
template<class scalar_type>
void ModelEvaluatorTPETRA<scalar_type>::finalize()
{
  auto comm_ = Teuchos::DefaultComm<int>::getComm(); 
  int mypid = comm_->getRank();
  int numproc = comm_->getSize();

  bool dorestart = paramList.get<bool> (TusasrestartNameString);

  write_exodus();

  std::cout<<(solver_->getList()).sublist("Direction").sublist("Newton").sublist("Linear Solver")<<std::endl;
  int ngmres = 0;

  if ( (solver_->getList()).sublist("Direction").sublist("Newton").sublist("Linear Solver")
       .getEntryPtr("Total Number of Linear Iterations") != NULL)
    ngmres = ((solver_->getList()).sublist("Direction").sublist("Newton").sublist("Linear Solver")
	      .getEntry("Total Number of Linear Iterations")).getValue(&ngmres);
  if( 0 == mypid ){
    int numstep = paramList.get<int> (TusasntNameString) - this->start_step;
    std::cout<<std::endl
	     <<"Total number of Newton iterations:     "<<nnewt_<<std::endl
	     <<"Total number of GMRES iterations:      "<<ngmres<<std::endl 
	     <<"Total number of Timesteps:             "<<numstep<<std::endl
	     <<"Average number of Newton per Timestep: "<<(float)nnewt_/(float)(numstep)<<std::endl
	     <<"Average number of GMRES per Newton:    "<<(float)ngmres/(float)nnewt_<<std::endl
	     <<"Average number of GMRES per Timestep:  "<<(float)ngmres/(float)(numstep)<<std::endl;
    if( dorestart ) std::cout<<"============THIS IS A RESTARTED RUN============"<<std::endl;
    std::ofstream outfile;
    outfile.open("jfnk.dat");
    outfile 
      <<"Total number of Newton iterations:     "<<nnewt_<<std::endl
      <<"Total number of GMRES iterations:      "<<ngmres<<std::endl 
      <<"Total number of Timesteps:             "<<numstep<<std::endl
      <<"Average number of Newton per Timestep: "<<(float)nnewt_/(float)(numstep)<<std::endl
      <<"Average number of GMRES per Newton:    "<<(float)ngmres/(float)nnewt_<<std::endl
      <<"Average number of GMRES per Timestep:  "<<(float)ngmres/(float)(numstep)<<std::endl; 
    if( dorestart ) outfile<<"============THIS IS A RESTARTED RUN============"<<std::endl;	
    outfile.close();
  }

#if 0
  if(!x_space_.is_null()) x_space_=Teuchos::null;
  if(!x_owned_map_.is_null()) x_owned_map_=Teuchos::null;
  if(!f_owned_map_.is_null()) f_owned_map_=Teuchos::null;
  if(!W_graph_.is_null()) W_graph_=Teuchos::null;
  if(!W_factory_.is_null()) W_factory_=Teuchos::null;
  if(!x0_.is_null()) x0_=Teuchos::null;
  if(!P_.is_null())  P_=Teuchos::null;
  if(!prec_.is_null()) prec_=Teuchos::null;
  //if(!solver_.is_null()) solver_=Teuchos::null;
  if(!u_old_.is_null()) u_old_=Teuchos::null;
  if(!dudt_.is_null()) dudt_=Teuchos::null;

  delete residualfunc_;
  delete preconfunc_;
  delete initfunc_;
#endif
  delete varnames_;
#if 0
  if( NULL != dirichletfunc_) delete dirichletfunc_;
#ifdef PERIODIC_BC
#else
  if( NULL != periodicbc_) delete periodicbc_;
#endif
  //if( NULL != postprocfunc_) delete postprocfunc_;
#endif
}



#endif