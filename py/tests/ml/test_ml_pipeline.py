#!/usr/bin/env python3
"""ML/AI管道测试."""

from __future__ import annotations

from datetime import date, datetime

import pandas as pd
import pytest

from quant_invest.ml import (
    EvaluationMetrics,
    MLPipeline,
    ModelEvaluator,
    ModelVersion,
    ModelVersioning,
    TrainConfig,
)
from quant_invest.ml.feature import FeatureBuilder, FeatureConfig, FeatureTransformer


class TestFeatureConfig:
    """特征配置测试."""

    def test_create_config(self):
        """创建配置."""
        config = FeatureConfig(name="momentum_20", transform="zscore", params={"window": 20})
        assert config.name == "momentum_20"
        assert config.transform == "zscore"

    def test_config_defaults(self):
        """默认值."""
        config = FeatureConfig(name="test")
        assert config.source == "python_calc"
        assert config.transform == "raw"
        assert config.lookback == 0
        assert config.params is None


class TestFeatureBuilder:
    """特征构建器测试."""

    def test_empty_build(self):
        """无特征配置."""
        builder = FeatureBuilder()
        X, y = builder.build(["000001.SZ"], date(2024, 1, 2), date(2024, 1, 10))
        assert X.empty
        assert len(y) == 0

    def test_build_single_feature(self):
        """单个特征."""
        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="momentum", transform="raw"))
        X, y = builder.build(["000001.SZ"], date(2024, 1, 2), date(2024, 1, 10))
        assert not X.empty
        assert "momentum" in X.columns
        assert len(y) == len(X)

    def test_build_multiple_features(self):
        """多个特征."""
        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="momentum", transform="raw"))
        builder.add_feature(FeatureConfig(name="volatility", transform="zscore"))
        builder.add_feature(FeatureConfig(name="volume", transform="rank"))
        X, y = builder.build(["000001.SZ"], date(2024, 1, 2), date(2024, 1, 10))
        assert len(X.columns) == 3

    def test_build_multiple_symbols(self):
        """多标的."""
        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="momentum"))
        X, y = builder.build(
            ["000001.SZ", "600000.SH"], date(2024, 1, 2), date(2024, 1, 10)
        )
        assert not X.empty
        assert isinstance(X.index, pd.MultiIndex)

    def test_add_features(self):
        """批量添加."""
        builder = FeatureBuilder()
        configs = [
            FeatureConfig(name=f"feature_{i}") for i in range(5)
        ]
        builder.add_features(configs)
        assert len(builder._feature_configs) == 5

    def test_feature_names(self):
        """特征名称列表."""
        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="alpha"))
        builder.add_feature(FeatureConfig(name="beta"))
        assert builder.feature_names == ["alpha", "beta"]

    def test_split_time_series(self):
        """时序切分."""
        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="test", transform="raw"))
        X, y = builder.build(["000001.SZ"], date(2024, 1, 2), date(2024, 1, 31))
        result = builder.split_time_series(X, y, train_ratio=0.6, valid_ratio=0.2)
        X_train, y_train, X_valid, y_valid, X_test, y_test = result
        assert len(X_train) > 0
        assert len(X_valid) > 0
        assert len(X_test) > 0
        total = len(X_train) + len(X_valid) + len(X_test)
        assert total == len(X)


class TestFeatureTransformer:
    """特征变换测试."""

    def test_zscore(self):
        """Z-score标准化."""
        dates = pd.bdate_range("2024-01-02", "2024-01-05")
        index = pd.MultiIndex.from_product([dates, ["A", "B"]], names=["date", "symbol"])
        data = {"value": [10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0]}
        df = pd.DataFrame(data, index=index)
        result = FeatureTransformer.zscore(df, groupby="date")
        assert not result.isnull().any().any()

    def test_rank(self):
        """排名."""
        dates = pd.bdate_range("2024-01-02", "2024-01-02")
        index = pd.MultiIndex.from_product(
            [dates, ["A", "B", "C", "D"]], names=["date", "symbol"]
        )
        df = pd.DataFrame({"a": [1.0, 2.0, 3.0, 4.0]}, index=index)
        result = FeatureTransformer.rank(df)
        assert result.iloc[0, 0] == pytest.approx(0.25)
        assert result.iloc[-1, 0] == pytest.approx(1.0)

    def test_winsorize(self):
        """去极值."""
        df = pd.DataFrame({"a": [1.0, 2.0, 10000.0, 4.0]})
        result = FeatureTransformer.winsorize(df, n_sigma=0.5)
        assert result.iloc[2, 0] < 10000.0
        assert result.iloc[0, 0] >= 1.0

    def test_fillna_forward(self):
        """前向填充."""
        df = pd.DataFrame({"a": [1.0, None, 3.0]})
        result = FeatureTransformer.fillna(df, method="ffill")
        assert result.iloc[1, 0] == 1.0

    def test_fillna_mean(self):
        """均值填充."""
        df = pd.DataFrame({"a": [1.0, None, 3.0]})
        result = FeatureTransformer.fillna(df, method="mean")
        assert result.iloc[1, 0] == 2.0


class TestModelEvaluator:
    """模型评估测试."""

    def test_evaluate_metrics(self):
        """评估指标计算."""
        from sklearn.linear_model import LinearRegression

        n = 100
        X = pd.DataFrame({"feature": range(n)})
        y = pd.Series([i * 0.5 + (i % 3) * 0.1 for i in range(n)])

        model = LinearRegression()
        model.fit(X, y)

        evaluator = ModelEvaluator()
        metrics = evaluator.evaluate(model, X, y)
        assert metrics.mse >= 0
        assert metrics.rmse >= 0
        assert metrics.mae >= 0

    def test_evaluate_r2(self):
        """R2计算."""
        from sklearn.linear_model import LinearRegression

        X = pd.DataFrame({"feature": [1.0, 2.0, 3.0, 4.0, 5.0]})
        y = pd.Series([2.0, 4.0, 6.0, 8.0, 10.0])

        model = LinearRegression()
        model.fit(X, y)

        evaluator = ModelEvaluator()
        metrics = evaluator.evaluate(model, X, y)
        assert abs(metrics.r2 - 1.0) < 0.1

    def test_rank_ic(self):
        """Rank IC."""
        pred = pd.Series([1.0, 2.0, 3.0, 4.0])
        actual = pd.Series([2.0, 1.0, 4.0, 3.0])
        ic = ModelEvaluator.calc_rank_ic(pred, actual)
        assert ic != 0.0


class TestModelVersioning:
    """模型版本管理测试."""

    def test_register_version(self):
        """注册版本."""
        registry = ModelVersioning()
        version = registry.register(
            model=None,
            metrics={"rmse": 0.1},
            config={"model_type": "xgboost"},
            feature_list=["feature_1", "feature_2"],
        )
        assert version.version_id == "v1"
        assert version.model_type == "xgboost"
        assert not version.is_production

    def test_list_versions(self):
        """列出版本."""
        registry = ModelVersioning()
        registry.register(None, {"rmse": 0.1}, {"model_type": "xgboost"}, ["f1"])
        registry.register(None, {"rmse": 0.2}, {"model_type": "xgboost"}, ["f1"])
        assert len(registry.list_versions()) == 2

    def test_promote(self):
        """提升为生产版本."""
        registry = ModelVersioning()
        version = registry.register(None, {}, {"model_type": "xgboost"}, [])
        registry.promote(version.version_id)
        assert version.is_production

    def test_get_production(self):
        """获取生产版本."""
        registry = ModelVersioning()
        registry.register(None, {}, {"model_type": "lstm"}, [])
        v2 = registry.register(None, {}, {"model_type": "xgboost"}, [])
        registry.promote(v2.version_id)

        prod = registry.get_production("xgboost")
        assert prod is not None
        assert prod.version_id == v2.version_id

        prod_lstm = registry.get_production("lstm")
        assert prod_lstm is None

    def test_list_by_type(self):
        """按类型过滤."""
        registry = ModelVersioning()
        registry.register(None, {}, {"model_type": "xgboost"}, [])
        registry.register(None, {}, {"model_type": "lstm"}, [])
        xgb_versions = registry.list_versions(model_type="xgboost")
        assert len(xgb_versions) == 1


class TestMLPipeline:
    """ML管道测试."""

    def test_train_no_model(self):
        """无模型."""
        pipeline = MLPipeline()
        config = TrainConfig(
            train_start=date(2024, 1, 2),
            train_end=date(2024, 2, 2),
            valid_start=date(2024, 2, 3),
            valid_end=date(2024, 3, 2),
            test_start=date(2024, 3, 3),
            test_end=date(2024, 4, 2),
        )
        result = pipeline.train(["000001.SZ"], config)
        assert result["model"] is None

    def test_train_no_builder(self):
        """无特征构建器."""
        from sklearn.linear_model import LinearRegression

        pipeline = MLPipeline(model=LinearRegression())
        config = TrainConfig(
            train_start=date(2024, 1, 2),
            train_end=date(2024, 2, 2),
            valid_start=date(2024, 2, 3),
            valid_end=date(2024, 3, 2),
            test_start=date(2024, 3, 3),
            test_end=date(2024, 4, 2),
        )
        result = pipeline.train(["000001.SZ"], config)
        assert result["model"] is not None

    def test_train_with_builder(self):
        """完整训练."""
        from sklearn.linear_model import LinearRegression

        builder = FeatureBuilder()
        builder.add_feature(FeatureConfig(name="feature_1", transform="raw"))
        builder.add_feature(FeatureConfig(name="feature_2", transform="zscore"))

        pipeline = MLPipeline(
            feature_builder=builder,
            model=LinearRegression(),
        )
        config = TrainConfig(
            train_start=date(2024, 1, 2),
            train_end=date(2024, 2, 2),
            valid_start=date(2024, 2, 3),
            valid_end=date(2024, 3, 2),
            test_start=date(2024, 3, 3),
            test_end=date(2024, 4, 2),
        )
        result = pipeline.train(["000001.SZ", "600000.SH"], config)
        assert "metrics" in result

    def test_predict_no_model(self):
        """无模型预测."""
        pipeline = MLPipeline()
        preds = pipeline.predict(["000001.SZ"], date(2024, 1, 10))
        assert len(preds) == 0


class TestEvaluationMetrics:
    """评估指标数据类测试."""

    def test_defaults(self):
        """默认值."""
        metrics = EvaluationMetrics()
        assert metrics.mse == 0.0
        assert metrics.rank_ic == 0.0

    def test_custom_values(self):
        """自定义值."""
        metrics = EvaluationMetrics(mse=0.01, rmse=0.1, r2=0.85)
        assert metrics.mse == 0.01
        assert metrics.rmse == 0.1
        assert metrics.r2 == 0.85


class TestModelVersion:
    """模型版本数据类测试."""

    def test_create_version(self):
        """创建版本."""
        now = datetime.now()
        version = ModelVersion(
            version_id="v1",
            model_type="xgboost",
            created_at=now,
            metrics={"rmse": 0.1},
            config={"learning_rate": 0.01},
            feature_list=["f1", "f2"],
            file_path="/tmp/model.pkl",
        )
        assert version.version_id == "v1"
        assert version.model_type == "xgboost"


class TestTrainConfig:
    """训练配置测试."""

    def test_create_config(self):
        """创建配置."""
        config = TrainConfig(
            train_start=date(2024, 1, 1),
            train_end=date(2024, 6, 30),
            valid_start=date(2024, 7, 1),
            valid_end=date(2024, 8, 31),
            test_start=date(2024, 9, 1),
            test_end=date(2024, 12, 31),
        )
        assert config.walk_forward
        assert config.retrain_interval == 20
