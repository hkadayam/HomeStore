pipeline {
    agent any

    environment {
        ORG = 'sds'
        PROJECT = 'homestore'
        CONAN_CHANNEL = 'develop'
        CONAN_USER = 'sds'
        CONAN_PASS = credentials('CONAN_PASS')
    }

    stages {
        stage('Get Version') {
            steps {
                script {
                    TAG = sh(script: "grep 'version =' conanfile.py | awk '{print \$3}' | tr -d '\n' | tr -d '\"'", returnStdout: true)
                }
            }
        }

        stage('Build') {
            steps {
                sh "docker build --rm --build-arg CONAN_USER=${CONAN_USER} --build-arg CONAN_PASS=${CONAN_PASS} --build-arg CONAN_CHANNEL=${CONAN_CHANNEL} --build-arg HOMESTORE_BUILD_TAG=${GIT_COMMIT} -t ${PROJECT}-${TAG} ."
            }
        }

        stage('Test') {
            steps {
                sh "docker rm -f ${PROJECT}_coverage || true"
                sh "docker create --name ${PROJECT}_coverage ${PROJECT}-${TAG}"
                sh "docker cp ${PROJECT}_coverage:/output/coverage.xml coverage.xml"
                cobertura autoUpdateHealth: false, autoUpdateStability: false, coberturaReportFile: 'coverage.xml', conditionalCoverageTargets: '20, 0, 0', fileCoverageTargets: '65, 0, 0', lineCoverageTargets: '45, 0, 0', maxNumberOfBuilds: 0, sourceEncoding: 'ASCII', zoomCoverageChart: false
            }
        }

        stage('Deploy') {
            when {
                branch "${CONAN_CHANNEL}"
            }
            steps {
                sh "docker run --rm ${PROJECT}-${TAG}"
                slackSend channel: '#conan-pkgs', message: "*${PROJECT}/${TAG}@${CONAN_USER}/${CONAN_CHANNEL}* has been uploaded to conan repo."
                withDockerRegistry([credentialsId: 'sds+sds', url: "https://ecr.vip.ebayc3.com"]) {
                    sh "docker build -f Dockerfile.test --rm --build-arg CONAN_USER=${CONAN_USER} --build-arg CONAN_PASS=${CONAN_PASS} --build-arg CONAN_CHANNEL=${CONAN_CHANNEL} --build-arg HOMESTORE_BUILD_TAG=${GIT_COMMIT} -t ${PROJECT}-${TAG}-test ."
                    sh "docker tag ${PROJECT}-${TAG}-test ecr.vip.ebayc3.com/${ORG}/${PROJECT}:${CONAN_CHANNEL}-test"
                    sh "docker push ecr.vip.ebayc3.com/${ORG}/${PROJECT}:${CONAN_CHANNEL}-test"
                    sh "docker rmi ecr.vip.ebayc3.com/${ORG}/${PROJECT}:${CONAN_CHANNEL}-test"
                    slackSend channel: '#conan-pkgs', message: "*${PROJECT}:${CONAN_CHANNEL}-test* has been pushed to ECR."
                }
            }
        }
    }

    post {
        always {
            sh "docker rm -f ${PROJECT}_coverage || true"
            sh "docker rmi -f ${PROJECT}-${TAG}"
        }
    }
}
