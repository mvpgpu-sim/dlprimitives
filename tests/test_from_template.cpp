#include <dlprim/operator.hpp>
#include <dlprim/json.hpp>
#include <fstream>
#include <iostream>
#include "test.hpp"

namespace dp = dlprim;


std::vector<dp::TensorSpecs> tensor_specs_from_json(dp::json::value const &v)
{
    std::vector<dp::TensorSpecs> r;
    dp::json::array const &ts = v.array();
    for(dp::json::value const &spec : ts) {
        std::vector<int> vsp = spec.get<std::vector<int> >("shape");
        dp::Shape sp=dp::Shape::from_range(vsp.begin(),vsp.end());
        dp::DataType dtype = dp::string_to_data_type(spec.get<std::string>("dtype","float"));
        r.push_back(dp::TensorSpecs(sp,dtype));
    }
    return r;
}

std::vector<dp::Shape> tensor_shapes_from_json(dp::json::value const &v)
{
    std::vector<dp::Shape> r;
    dp::json::array const &sps = v.array();
    for(dp::json::value const &jsp : sps) {
        std::vector<int> vsp = jsp.get_value<std::vector<int> >();
        dp::Shape sp=dp::Shape::from_range(vsp.begin(),vsp.end());
        r.push_back(sp);
    }
    return r;
}

std::vector<dp::Tensor> make_tensors(dp::Context &ctx,
                                     std::vector<dp::TensorSpecs> const &specs)
{
    std::vector<dp::Tensor> r;
    for(size_t i=0;i<specs.size();i++) {
        r.push_back(dp::Tensor(ctx,specs[i].shape(),specs[i].dtype()));
    }
    return r;
}


std::vector<dp::Tensor> make_tensors(dp::Context &ctx,
                                     std::vector<dp::Shape> const &shapes,
                                     std::vector<dp::TensorSpecs> const &specs)
{
    std::vector<dp::Tensor> r;
    for(size_t i=0;i<shapes.size();i++) {
        r.push_back(dp::Tensor(ctx,shapes[i],specs[i].dtype()));
    }
    return r;
}

void copy_tensors(std::vector<dp::Tensor> &tensors,dp::json::value const &v)
{
    dp::json::array const &vals = v.array();
    for(size_t i=0;i<tensors.size();i++) {
        std::vector<float> const &values = vals.at(i).get_value<std::vector<float> >();
        if(tensors[i].dtype() == dp::float_data) {
            float *p=tensors[i].data<float>();
            memcpy(p,values.data(),sizeof(float)*tensors[i].shape().total_size());
        }
        else {
            TEST(!"Unsuported data type");
        }
    }
}

void copy_tensors(std::vector<dp::Tensor> &out,std::vector<dp::Tensor> &inp,dp::Context &ctx)
{
    TESTEQ(out.size(),inp.size());
    cl::CommandQueue q = ctx.make_queue();
        
    for(size_t i=0;i<out.size();i++) {
        TESTEQ(out[i].shape(),inp[i].shape());
        TESTEQ(out[i].dtype(),inp[i].dtype());
        size_t len = dp::size_of_data_type(out[i].dtype()) * out[i].shape().total_size();
        memcpy(out[i].host_data(),inp[i].host_data(),len);
        out[i].to_device(q,true);
    }
}


void compare_tensors(std::vector<dp::Tensor> &actual,std::vector<dp::Tensor> &reference,float eps)
{
    for(size_t i=0;i < actual.size();i++) {
        if(actual[i].dtype() == dp::float_data) {
            float *a=actual[i].data<float>();
            float *r=reference[i].data<float>();
            int total =  actual[i].shape().total_size();
            for(int j=0;j<total;j++) {
                if(fabs(a[j] - r[j]) > eps) {
                    std::ostringstream err;
                    err << "Comparison failed for tensor #" << i << " at " << j << " expecting " << r[j] << " got " << a[j] << " for esp="<<eps;
                    throw std::runtime_error(err.str());
                }
            }
        }
        else {
            TEST(!"Unsuported data type");
        }
    }
}

void fill_random(dp::Tensor &t)
{
    TEST(t.dtype() == dp::float_data);
    float *p = t.data<float>();
    for(size_t i=0;i<t.shape().total_size();i++) {
        p[i]=random()*2.0f / RAND_MAX - 1.0f;
    }
}

void fill_small_int(dp::Tensor &t,int range,float factor = 1.0)
{
    TEST(t.dtype() == dp::float_data);
    float *p = t.data<float>();
    for(size_t i=0;i<t.shape().total_size();i++) {
        int v = int(double(random()) / RAND_MAX * range) - range / 2;
        p[i]= v * factor;
    }
}


void initialize_tensors(std::vector<dp::Tensor> &tensors,std::string const &op)
{
    for(auto &t:tensors) {
        if(op == "uniform")
            fill_random(t);
        else if(op == "small_int")
            fill_small_int(t,5,1.0f);
        else if(op == "small_frac")
            fill_small_int(t,5,0.25f);
        else
            throw std::runtime_error("Unknown methods " + op);
    }
}

int main(int argc,char **argv)
{
    if(argc != 3) {
        std::cerr << "Usage test_from_template context_id test.json" << std::endl;
        return 1;
    }
    try {
        dp::Context cpu_ctx;
        dp::ExecutionContext cpu_e;

        dp::Context ctx(argv[1]);
        cl::CommandQueue q = ctx.make_queue();
        dp::ExecutionContext e(q);
        
        std::ifstream f(argv[2]);
        if(!f) {
            std::cerr << "Failed to open " << argv[2] << std::endl;
            return 1;
        }
        dp::json::value config;
        int line = -1;
        if(!config.load(f,true,&line)) {
            std::cerr << "Failed to load json template " << argv[2] << " syntax error at " << line << std::endl;
            return 1;
        }
        
        std::string op_name = config.get<std::string>("operator");
        std::cout << "Running tests for operator " << op_name << " on " << ctx.name() << std::endl;
        dp::json::array &tests = config["tests"].array();
        for(size_t i=0;i<tests.size();i++) {
            std::cout << "- Running test case " << i << " for options " << tests[i]["options"] << std::endl;
            std::string rnd = tests[i].get("init","uniform");
            std::unique_ptr<dp::Operator> op =     dp::create_by_name(ctx,    op_name,tests[i]["options"]);
            std::unique_ptr<dp::Operator> ref_op = dp::create_by_name(cpu_ctx,op_name,tests[i]["options"]);
            dp::OperatorWithParameters *pop = dynamic_cast<dp::OperatorWithParameters*>(op.get());

            std::vector<dp::TensorSpecs> input_specs = tensor_specs_from_json(tests[i]["setup_tensors"]);
            std::vector<dp::TensorSpecs> output_specs = tensor_specs_from_json(tests[i]["output_tensors"]);
            int ws = tests[i].get("workspace",-1);
            std::vector<dp::TensorSpecs> res_specs;
            size_t res_ws;
            op->setup(input_specs,res_specs,res_ws);
            TEST(res_specs == output_specs);
            TEST(ws == -1 || res_ws == size_t(ws));
            if(res_ws > 0) {
                op->set_workspace(dp::Tensor(ctx,dp::Shape(res_ws),dp::uint8_data));
            }
            size_t ref_ws;
            ref_op->setup(input_specs,res_specs,ref_ws);
            if(ref_ws) {
                ref_op->set_workspace(dp::Tensor(cpu_ctx,dp::Shape(ref_ws),dp::uint8_data));
            }
            TEST(res_specs == output_specs);
            if(!tests[i].find("param_specs").is_undefined()) {
                TEST(pop != nullptr);
                std::vector<dp::TensorSpecs> param_specs = tensor_specs_from_json(tests[i]["param_specs"]);
                TEST(pop->parameter_specs() == param_specs);
                auto &params = pop->parameters();
                auto &ref_params = dynamic_cast<dp::OperatorWithParameters &>(*ref_op).parameters();
                params = make_tensors(ctx,param_specs);
                ref_params = make_tensors(cpu_ctx,param_specs);
                if(!tests[i].get("random_params",false)) {
                    copy_tensors(ref_params,tests[i]["param_tensors"]);
                }
                else {
                    initialize_tensors(ref_params,rnd);
                }
                copy_tensors(params,ref_params,ctx);
            }
            dp::json::array const &cases = tests[i]["cases"].array();
            for(size_t i=0;i<cases.size();i++) {
                std::cout << "-- test for shape " << cases[i]["in_shapes"] << std::endl;
                std::vector<dp::Shape> in_shapes = tensor_shapes_from_json(cases[i]["in_shapes"]);
                std::vector<dp::Shape> out_shapes = tensor_shapes_from_json(cases[i]["out_shapes"]);
                std::vector<dp::Shape> res_shape;
                op->reshape(in_shapes,res_shape);
                TEST(out_shapes == res_shape);
                std::vector<dp::Tensor> in_tensors = make_tensors(ctx,in_shapes,input_specs);
                std::vector<dp::Tensor> out_tensors = make_tensors(ctx,out_shapes,output_specs);
                std::vector<dp::Tensor> ref_tensors = make_tensors(cpu_ctx,out_shapes,output_specs);
                if(!cases[i].get("use_cpu_reference",false)) {
                    copy_tensors(in_tensors,cases[i]["in_tensors"]);
                    copy_tensors(ref_tensors,cases[i]["out_tensors"]);
                }
                else {
                    initialize_tensors(in_tensors,rnd);
                    ref_op->forward(in_tensors,ref_tensors,cpu_e);
                }
                if(ctx.is_gpu_context()) {
                    for(dp::Tensor &tensor : in_tensors)
                        tensor.to_device(e,false);
                }
                op->forward(in_tensors,out_tensors,e);
                if(ctx.is_gpu_context()) {
                    for(dp::Tensor &tensor : out_tensors)
                        tensor.to_host(e,false);
                    if(ctx.is_gpu_context())    
                        e.queue().finish();
                }
                compare_tensors(out_tensors,ref_tensors,cases[i].get<double>("eps",1e-5));
            }
        }

        std::cout << "Testing " << argv[2] << " completed sucesefully" << std::endl;
        
        return 0;
    }
    catch(cl::Error const &e) {
        std::cerr << "FAILED: " << e.what() << " " << e.err()<< std::endl;
        return 1;
    }
    catch(std::exception const &e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }
    
}
