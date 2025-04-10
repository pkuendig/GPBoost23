context("gpb.interpete")

# Avoid that long tests get executed on CRAN
if(Sys.getenv("GPBOOST_ALL_TESTS") == "GPBOOST_ALL_TESTS"){
  
  .sigmoid <- function(x) {
    1.0 / (1.0 + exp(-x))
  }
  .logit <- function(x) {
    log(x / (1.0 - x))
  }
  
  test_that("gpb.intereprete works as expected for binary classification", {
    data(agaricus.train, package = "gpboost")
    train <- agaricus.train
    dtrain <- gpb.Dataset(train$data, label = train$label)
    setinfo(
      dataset = dtrain
      , "init_score"
      , rep(
        .logit(mean(train$label))
        , length(train$label)
      )
    )
    data(agaricus.test, package = "gpboost")
    test <- agaricus.test
    params <- list(
      objective = "binary"
      , learning_rate = 0.01
      , num_leaves = 63L
      , max_depth = -1L
      , min_data_in_leaf = 1L
      , min_sum_hessian_in_leaf = 1.0
    )
    model <- gpb.train(
      params = params
      , data = dtrain
      , nrounds = 3L
      , verbose = 0
    )
    num_trees <- 5L
    tree_interpretation <- gpb.interprete(
      model = model
      , data = test$data
      , idxset = seq_len(num_trees)
    )
    expect_identical(class(tree_interpretation), "list")
    expect_true(length(tree_interpretation) == num_trees)
    expect_null(names(tree_interpretation))
    expect_true(all(
      sapply(
        X = tree_interpretation
        , FUN = function(treeDT) {
          checks <- c(
            data.table::is.data.table(treeDT)
            , identical(names(treeDT), c("Feature", "Contribution"))
            , is.character(treeDT[, Feature])
            , is.numeric(treeDT[, Contribution])
          )
          return(all(checks))
        }
      )
    ))
  })
  
  test_that("gpb.intereprete works as expected for multiclass classification", {
    data(iris)
    
    # We must convert factors to numeric
    # They must be starting from number 0 to use multiclass
    # For instance: 0, 1, 2, 3, 4, 5...
    iris$Species <- as.numeric(as.factor(iris$Species)) - 1L
    
    # Create imbalanced training data (20, 30, 40 examples for classes 0, 1, 2)
    train <- as.matrix(iris[c(1L:20L, 51L:80L, 101L:140L), ])
    # The 10 last samples of each class are for validation
    test <- as.matrix(iris[c(41L:50L, 91L:100L, 141L:150L), ])
    dtrain <- gpb.Dataset(data = train[, 1L:4L], label = train[, 5L])
    dtest <- gpb.Dataset.create.valid(dtrain, data = test[, 1L:4L], label = test[, 5L])
    params <- list(
      objective = "multiclass"
      , metric = "multi_logloss"
      , num_class = 3L
      , learning_rate = 0.00001
    )
    model <- gpb.train(
      params = params
      , data = dtrain
      , nrounds = 3L
      , min_data = 1L
      , verbose = 0
    )
    num_trees <- 5L
    tree_interpretation <- gpb.interprete(
      model = model
      , data = test[, 1L:4L]
      , idxset = seq_len(num_trees)
    )
    expect_identical(class(tree_interpretation), "list")
    expect_true(length(tree_interpretation) == num_trees)
    expect_null(names(tree_interpretation))
    expect_true(all(
      sapply(
        X = tree_interpretation
        , FUN = function(treeDT) {
          checks <- c(
            data.table::is.data.table(treeDT)
            , identical(names(treeDT), c("Feature", "Class 0", "Class 1", "Class 2"))
            , is.character(treeDT[, Feature])
            , is.numeric(treeDT[, `Class 0`])
            , is.numeric(treeDT[, `Class 1`])
            , is.numeric(treeDT[, `Class 2`])
          )
          return(all(checks))
        }
      )
    ))
  })
  
}